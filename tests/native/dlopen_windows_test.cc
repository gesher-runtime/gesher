// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Compile the actual production dlopen implementation. All loading, path
// resolution, module ownership and DLL imports use the real Windows APIs.
// Two API-boundary wrappers only count calls and inject explicit failure/CWD
// interleavings; they are compiled into this test, never into gesher_posix.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "../../compat/posix/src/internal.h"

namespace dlopen_test {
enum class PathControl { kNone, kFailure, kInsufficient, kEqualCapacity, kChangeDirectory };
PathControl path_control = PathControl::kNone;
unsigned path_calls = 0;
unsigned load_calls = 0;
DWORD load_error = ERROR_SUCCESS;
std::wstring next_directory;
bool changed_directory = false;

DWORD WINAPI FullPath(LPCWSTR path, DWORD capacity, LPWSTR output, LPWSTR* part) {
  ++path_calls;
  switch (path_control) {
    case PathControl::kFailure:
      SetLastError(ERROR_ACCESS_DENIED);
      return 0;
    case PathControl::kInsufficient:
    case PathControl::kEqualCapacity:
      if (capacity == 0) return 16;
      if (output != nullptr) output[0] = L'\0';
      return capacity + (path_control == PathControl::kInsufficient ? 1 : 0);
    default:
      break;
  }
  const DWORD length = GetFullPathNameW(path, capacity, output, part);
  if (path_control == PathControl::kChangeDirectory && path_calls == 1) {
    changed_directory = SetCurrentDirectoryW(next_directory.c_str()) != FALSE;
  }
  return length;
}

HMODULE WINAPI Load(LPCWSTR path, HANDLE file, DWORD flags) {
  ++load_calls;
  if (load_error != ERROR_SUCCESS) {
    SetLastError(load_error);
    return nullptr;
  }
  return LoadLibraryExW(path, file, flags);
}
}  // namespace dlopen_test

#define GetFullPathNameW dlopen_test::FullPath
#define LoadLibraryExW dlopen_test::Load
#include "../../compat/posix/src/dlfcn.cc"
#undef LoadLibraryExW
#undef GetFullPathNameW

namespace dlopen_test {
constexpr wchar_t kFixture[] = L"gesher-dlopen-fixture.dll";
constexpr wchar_t kDependency[] = L"gesher-dlopen-dependency.dll";
int checks = 0;
int failures = 0;

void Check(bool result, const char* description) {
  ++checks;
  if (!result) {
    ++failures;
    printf("FAIL dlopen: %s (Windows error %lu)\n", description, GetLastError());
  }
}

std::wstring CurrentDirectory() {
  std::vector<wchar_t> path(32768);
  const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(path.size()), path.data());
  return length > 0 && length < path.size() ? std::wstring(path.data(), length) : L"";
}

std::wstring ModulePath(void* handle) {
  if (handle == nullptr) return {};
  std::vector<wchar_t> path(32768);
  const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(handle), path.data(),
                                        static_cast<DWORD>(path.size()));
  return length > 0 && length < path.size() ? std::wstring(path.data(), length) : L"";
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
  return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void ResetControl(PathControl control = PathControl::kNone) {
  path_control = control;
  path_calls = 0;
  load_calls = 0;
  load_error = ERROR_SUCCESS;
  changed_directory = false;
  while (dlerror() != nullptr) {}
}

bool CopyFixtures(const std::wstring& source, const std::wstring& target) {
  return CopyFileW((source + L"\\" + kFixture).c_str(),
                   (target + L"\\" + kFixture).c_str(), TRUE) &&
         CopyFileW((source + L"\\" + kDependency).c_str(),
                   (target + L"\\" + kDependency).c_str(), TRUE);
}

void CheckLoad(const std::string& name, const std::wstring& expected, const char* description) {
  void* handle = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* error = dlerror();
    printf("dlopen diagnostic: %s\n", error != nullptr ? error : "(none)");
  }
  Check(handle != nullptr, description);
  if (handle == nullptr) return;
  Check(SamePath(ModulePath(handle), expected), "the requested file is the loaded module");
  using Value = int (*)();
  Value value = reinterpret_cast<Value>(dlsym(handle, "gesher_dlopen_value"));
  Check(value != nullptr && value() == 74, "real sibling DLL import executes");
  Check(dlclose(handle) == 0, "loaded module reference closes");
  Check(GetModuleHandleW(expected.c_str()) == nullptr, "last reference unloads fixture DLL");
}

int builtin_value = 91;
int elf_handle = 0;
int elf_symbol = 0;
unsigned elf_opens = 0;
unsigned elf_closes = 0;
std::string elf_name;
int elf_flags = 0;

void* OpenElfFixture(const char* name, int flags, char*, size_t) {
  ++elf_opens;
  elf_name = name;
  elf_flags = flags;
  return &elf_handle;
}
void* SymbolElfFixture(void* handle, const char* name) {
  return handle == &elf_handle && strcmp(name, "elf_symbol") == 0 ? &elf_symbol : nullptr;
}
int CloseElfFixture(void* handle) {
  if (handle != &elf_handle) return -1;
  ++elf_closes;
  return 0;
}

bool WriteElfMagic(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  const unsigned char magic[] = {0x7f, 'E', 'L', 'F'};
  DWORD written = 0;
  const bool ok = WriteFile(file, magic, sizeof(magic), &written, nullptr) &&
                  written == sizeof(magic);
  CloseHandle(file);
  return ok;
}

void Run(const std::wstring& source, const std::wstring& root) {
  const std::wstring plugins = root + L"\\plugins";
  const std::wstring child = root + L"\\child";
  const std::wstring unicode = root + L"\\space \u05e9\u05dc\u05d5\u05dd";
  const std::wstring other = root + L"\\a-longer-current-directory-for-the-resolution-interleaving";
  const bool setup = CreateDirectoryW(plugins.c_str(), nullptr) &&
      CreateDirectoryW(child.c_str(), nullptr) && CreateDirectoryW(unicode.c_str(), nullptr) &&
      CreateDirectoryW(other.c_str(), nullptr) && CopyFixtures(source, plugins) &&
      CopyFixtures(source, root) && CopyFixtures(source, unicode) &&
      SetCurrentDirectoryW(root.c_str());
  Check(setup, "create isolated real DLL fixtures");
  if (!setup) return;
  const std::wstring plugin_path = plugins + L"\\" + kFixture;
  const std::wstring root_path = root + L"\\" + kFixture;
  const std::string relative = "plugins\\gesher-dlopen-fixture.dll";

  ResetControl();
  void* absent = dlopen(relative.c_str(), RTLD_NOW | RTLD_NOLOAD);
  Check(absent == nullptr && load_calls == 0, "RTLD_NOLOAD never loads an absent module");
  Check(dlerror() != nullptr && dlerror() == nullptr, "dlerror is reported once");

  ResetControl();
  void* bare = dlopen("gesher-dlopen-fixture.dll", RTLD_NOW);
  Check(bare == nullptr, "bare names do not search the current directory");
  if (bare != nullptr) dlclose(bare);
  Check(path_calls == 0, "bare names preserve the safe loader search");
  const char* bare_error = dlerror();
  Check(bare_error != nullptr && strstr(bare_error, "gesher-dlopen-fixture.dll") != nullptr &&
            strstr(bare_error, "Windows error 126") != nullptr &&
            strstr(bare_error, "library not found") != nullptr,
        "missing bare library reports its name and real missing-module error");
  Check(dlerror() == nullptr, "missing-library error is consumed once");
  CheckLoad(".\\gesher-dlopen-fixture.dll", root_path,
            "explicit current-directory path loads the fixture bare-name search excluded");
  CheckLoad(relative, plugin_path, "relative backslash path loads with sibling dependency");
  CheckLoad("plugins/gesher-dlopen-fixture.dll", plugin_path,
            "relative slash path loads with sibling dependency");
  CheckLoad(gesher::Narrow(plugin_path.c_str()), plugin_path, "absolute path loads");
  CheckLoad(gesher::Narrow((unicode + L"\\" + kFixture).c_str()), unicode + L"\\" + kFixture,
            "UTF-8 path with spaces loads");

  Check(root.size() > 2 && root[1] == L':', "temporary fixture has a drive-qualified path");
  if (root.size() > 2 && root[1] == L':') {
    std::string drive_relative(1, static_cast<char>(root[0]));
    drive_relative += ":gesher-dlopen-fixture.dll";
    CheckLoad(drive_relative, root_path, "drive-relative name gets sibling dependency search");
    CheckLoad(gesher::Narrow(plugin_path.substr(2).c_str()), plugin_path,
              "root-relative path uses the current drive");
  }
  Check(SetCurrentDirectoryW(child.c_str()) != FALSE, "enter child directory");
  CheckLoad("../plugins/gesher-dlopen-fixture.dll", plugin_path, "parent-relative path loads");
  Check(SetCurrentDirectoryW(root.c_str()) != FALSE, "restore fixture working directory");

  ResetControl();
  void* system = dlopen("kernel32.dll", RTLD_NOW);
  Check(system != nullptr && path_calls == 0, "bare system DLL uses safe default search");
  Check(system != nullptr && dlsym(system, "GetCurrentProcessId") != nullptr,
        "symbol lookup on a real system DLL succeeds");
  if (system != nullptr) Check(dlclose(system) == 0, "system DLL reference closes");
  ResetControl();
  load_error = ERROR_INVALID_PARAMETER;
  void* invalid = dlopen("kernel32.dll", RTLD_NOW);
  Check(invalid == nullptr && path_calls == 0 && load_calls == 1,
        "injected bare-name loader failure reaches the Windows API boundary");
  const char* invalid_error = dlerror();
  Check(invalid_error != nullptr && strstr(invalid_error, "Windows error 87") != nullptr &&
            strstr(invalid_error, "library not found") == nullptr,
        "genuine invalid-parameter failure is not relabeled as a missing library");
  Check(dlerror() == nullptr, "invalid-parameter error is consumed once");
  if (invalid != nullptr) dlclose(invalid);
  ResetControl();
  DLL_DIRECTORY_COOKIE directory = AddDllDirectory(plugins.c_str());
  Check(directory != nullptr, "register an explicit user DLL directory");
  if (directory != nullptr) {
    CheckLoad("gesher-dlopen-fixture.dll", plugin_path, "bare name searches registered user directory");
    Check(RemoveDllDirectory(directory) != FALSE, "remove explicit user DLL directory");
  }

  ResetControl();
  void* owner = dlopen(relative.c_str(), RTLD_NOW);
  Check(owner != nullptr, "open original module owner");
  if (owner != nullptr) {
    const unsigned loads = load_calls;
    void* lookup = dlopen(relative.c_str(), RTLD_NOW | RTLD_NOLOAD);
    Check(lookup == owner && load_calls == loads, "RTLD_NOLOAD finds an existing module without loading");
    if (lookup != nullptr) Check(dlclose(lookup) == 0, "release owned RTLD_NOLOAD reference");
    const bool retained = GetModuleHandleW(plugin_path.c_str()) == owner;
    Check(retained, "closing RTLD_NOLOAD preserves the original owner's module");
    if (retained) {
      using Value = int (*)();
      Value value = reinterpret_cast<Value>(dlsym(owner, "gesher_dlopen_value"));
      Check(value != nullptr && value() == 74, "original owner's DLL remains callable");
      Check(dlclose(owner) == 0, "release original module owner");
    }
    Check(GetModuleHandleW(plugin_path.c_str()) == nullptr, "both owned references eventually unload");
  }

  for (const PathControl control : {PathControl::kFailure, PathControl::kInsufficient,
                                    PathControl::kEqualCapacity}) {
    ResetControl(control);
    void* failed = dlopen(relative.c_str(), RTLD_NOW);
    Check(failed == nullptr && load_calls == 0, "failed path resolution cannot fall back to DLL search");
    if (failed != nullptr) dlclose(failed);
    const char* error = dlerror();
    const std::string expected = "Windows error " + std::to_string(
        control == PathControl::kFailure ? ERROR_ACCESS_DENIED : ERROR_FILENAME_EXCED_RANGE);
    Check(error != nullptr && strstr(error, relative.c_str()) != nullptr &&
              strstr(error, expected.c_str()) != nullptr,
          "path failure keeps original filename and Windows error");
    Check(dlerror() == nullptr, "path failure is consumed once");
  }

  ResetControl(PathControl::kChangeDirectory);
  next_directory = other;
  CheckLoad(relative, plugin_path, "CWD change after resolution cannot redirect the requested DLL");
  Check(path_calls == 1 && changed_directory, "relative path is resolved once before CWD changes");
  Check(SetCurrentDirectoryW(root.c_str()) != FALSE, "restore CWD after controlled interleaving");
  ResetControl();

  void* self = dlopen(nullptr, RTLD_NOW);
  Check(self == GetModuleHandleW(nullptr) && dlclose(self) == 0, "null filename still denotes executable");
  static const gesher_builtin_symbol symbols[] = {{"builtin_value", &builtin_value}};
  gesher_register_builtin_library("gesher-dlopen-builtin.so", symbols, 1);
  void* builtin = dlopen("missing/directory/gesher-dlopen-builtin.so", RTLD_NOW);
  Check(builtin != nullptr && dlsym(builtin, "builtin_value") == &builtin_value &&
            dlclose(builtin) == 0 && path_calls == 0 && load_calls == 0,
        "builtin dispatch still precedes filesystem path resolution");

  Check(WriteElfMagic(root + L"\\elf-fixture.so"), "create explicit ELF dispatch fixture");
  void* elf_without_loader = dlopen(".\\elf-fixture.so", RTLD_NOW);
  const char* elf_error = dlerror();
  Check(elf_without_loader == nullptr && elf_error != nullptr &&
            strstr(elf_error, "no ELF loader") != nullptr,
        "ELF without a registered loader fails honestly");
  static const gesher_elf_dlopen_ops elf = {OpenElfFixture, SymbolElfFixture, CloseElfFixture};
  gesher_set_elf_dlopen(&elf);
  void* elf_result = dlopen(".\\elf-fixture.so", RTLD_NOW | RTLD_NOLOAD);
  Check(elf_result == &elf_handle && elf_opens == 1 && elf_name == ".\\elf-fixture.so" &&
            elf_flags == (RTLD_NOW | RTLD_NOLOAD) && path_calls == 0 && load_calls == 0,
        "ELF dispatch preserves original name and flags before Windows resolution");
  Check(dlsym(elf_result, "elf_symbol") == &elf_symbol && dlclose(elf_result) == 0 && elf_closes == 1,
        "registered ELF callbacks retain symbol and close ownership");
}
}  // namespace dlopen_test

int wmain(int argc, wchar_t** argv) {
  using namespace dlopen_test;
  if (argc != 2) {
    fprintf(stderr, "usage: dlopen_windows_test.exe <absolute fixture DLL directory>\n");
    return 2;
  }
  const std::wstring original = CurrentDirectory();
  wchar_t temporary[MAX_PATH + 1] = {};
  wchar_t root_buffer[MAX_PATH + 1] = {};
  const DWORD temporary_length = GetTempPathW(MAX_PATH + 1, temporary);
  const bool setup = !original.empty() && temporary_length > 0 && temporary_length <= MAX_PATH &&
      GetTempFileNameW(temporary, L"gdl", 0, root_buffer) && DeleteFileW(root_buffer) &&
      CreateDirectoryW(root_buffer, nullptr);
  Check(setup, "create test temporary directory");
  if (setup) {
    Run(argv[1], root_buffer);
    ResetControl();
    Check(SetCurrentDirectoryW(original.c_str()) != FALSE, "restore original working directory");
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path(root_buffer), error);
    Check(!error, "remove isolated fixture files and directories");
  }
  printf("dlopen Windows: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
