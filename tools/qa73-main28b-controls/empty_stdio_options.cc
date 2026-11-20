// Isolated observations only. The first case executes actual SpawnWithFds.
// Other cases vary documented CreateProcess setup around the same closed CRT
// block and unchanged ordinary-native child assertions; none is a product fix.
#include "exec_spawn.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool Retire(const char* name, DWORD error, PROCESS_INFORMATION* child) {
  DWORD wait = WAIT_FAILED, code = 127;
  BOOL exit_read = FALSE;
  if (!error) {
    CloseHandle(child->hThread);
    wait = WaitForSingleObject(child->hProcess, 10000);
    if (wait == WAIT_OBJECT_0) exit_read = GetExitCodeProcess(child->hProcess, &code);
    else { TerminateProcess(child->hProcess, 124); WaitForSingleObject(child->hProcess, 5000); }
    CloseHandle(child->hProcess);
  }
  std::printf("EMPTY_STDIO_RESULT name=%s create_error=%lu wait=%lu exit_read=%d code=%lu\n",
              name, error, wait, !!exit_read, code);
  return error == 0 && wait == WAIT_OBJECT_0 && exit_read && (code == 0 || code == 10);
}

static DWORD Direct(const std::wstring& image, DWORD flags, bool null_standard,
                    bool allowlist, PROCESS_INFORMATION* child) {
  const int count = 3;
  std::vector<unsigned char> block(sizeof(count) + count * (1 + sizeof(intptr_t)), 0);
  std::memcpy(block.data(), &count, sizeof(count));
  const intptr_t closed = -1;
  for (int fd = 0; fd < count; ++fd)
    std::memcpy(block.data() + sizeof(count) + count + fd * sizeof(intptr_t),
                &closed, sizeof(closed));
  HANDLE sentinel = nullptr;
  SIZE_T size = 0;
  std::vector<char> storage;
  LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
  if (allowlist) {
    // A bounded control owns exactly this one event, never an ambient handle.
    // It is not a proposed production solution or a standard stream.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    sentinel = CreateEventW(&sa, TRUE, FALSE, nullptr);
    if (!sentinel) return GetLastError();
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (!size) { DWORD e = GetLastError(); CloseHandle(sentinel); return e; }
    storage.resize(size);
    attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
      DWORD e = GetLastError(); CloseHandle(sentinel); return e;
    }
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    &sentinel, sizeof(sentinel), nullptr, nullptr)) {
      DWORD e = GetLastError(); DeleteProcThreadAttributeList(attributes); CloseHandle(sentinel); return e;
    }
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = allowlist ? sizeof(startup) : sizeof(startup.StartupInfo);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = null_standard ? nullptr : INVALID_HANDLE_VALUE;
  startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdInput;
  startup.StartupInfo.hStdError = startup.StartupInfo.hStdInput;
  startup.StartupInfo.cbReserved2 = static_cast<WORD>(block.size());
  startup.StartupInfo.lpReserved2 = block.data();
  startup.lpAttributeList = attributes;
  std::wstring command;
  gesher::exec::AppendQuoted(&command, image);
  command += L" --empty";
  flags |= CREATE_UNICODE_ENVIRONMENT;
  if (allowlist) flags |= EXTENDED_STARTUPINFO_PRESENT;
  const BOOL ok = CreateProcessW(image.c_str(), command.data(), nullptr, nullptr,
      allowlist ? TRUE : FALSE, flags, nullptr, nullptr, &startup.StartupInfo, child);
  DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  if (attributes) DeleteProcThreadAttributeList(attributes);
  if (sentinel) CloseHandle(sentinel);
  return error;
}

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) return 2;
  const std::wstring image = argv[1], output = argv[2];
  struct Case { const char* name; const wchar_t* file; DWORD flags; bool null_standard; bool allowlist; };
  const Case cases[] = {
    {"production", L"production.log", 0, false, false},
    {"no-window-invalid", L"no-window-invalid.log", CREATE_NO_WINDOW, false, false},
    {"detached-invalid", L"detached-invalid.log", DETACHED_PROCESS, false, false},
    {"no-window-null", L"no-window-null.log", CREATE_NO_WINDOW, true, false},
    {"detached-null", L"detached-null.log", DETACHED_PROCESS, true, false},
    {"explicit-allowlist", L"explicit-allowlist.log", CREATE_NO_WINDOW, false, true},
  };
  bool observed = true;
  for (const auto& item : cases) {
    const std::wstring log = output + L"\\" + item.file;
    if (!SetEnvironmentVariableW(L"GESHER_QA73_CHILD_LOG", log.c_str())) return 3;
    PROCESS_INFORMATION child{};
    DWORD error;
    if (std::strcmp(item.name, "production") == 0) {
      gesher::exec::Launch launch;
      launch.application = image; launch.kind = "exe";
      gesher::exec::AppendQuoted(&launch.command_line, image);
      launch.command_line += L" --empty";
      error = gesher::exec::SpawnWithFds(launch, nullptr, L"", {}, &child);
    } else error = Direct(image, item.flags, item.null_standard, item.allowlist, &child);
    const bool ok = Retire(item.name, error, &child);
    observed = ok && observed;
  }
  SetEnvironmentVariableW(L"GESHER_QA73_CHILD_LOG", nullptr);
  std::printf("EMPTY_STDIO_OBSERVATIONS_COMPLETE cases=6 completed=%d\n", observed);
  return observed ? 0 : 1;
}
