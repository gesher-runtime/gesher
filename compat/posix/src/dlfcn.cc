// gesher-base: <dlfcn.h> over LoadLibrary / GetProcAddress for Windows DLLs.
// Android ELF libraries go to the ELF loader registered through
// <gesher/elf_dlopen.h> (phase 6).

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <gesher/builtin_library.h>
#include <gesher/elf_dlopen.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "internal.h"

namespace {

thread_local std::string last_error;
thread_local bool has_error = false;

void SetError(const char* what, const char* name) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%s %s: Windows error %lu", what, name != nullptr ? name : "",
           GetLastError());
  last_error = buf;
  has_error = true;
}

// --- built-in libraries (<gesher/builtin_library.h>) --------------------------

struct BuiltinLibrary {
  std::string name;
  const gesher_builtin_symbol* symbols;
  size_t count;
};

std::mutex builtins_mutex;
// Never freed: handles point into it.
std::vector<BuiltinLibrary*>& Builtins() {
  static std::vector<BuiltinLibrary*> builtins;
  return builtins;
}

// The file name in a path, '/' or '\' separated.
std::string_view BaseName(std::string_view path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

BuiltinLibrary* FindBuiltin(const char* filename) {
  std::string_view base = BaseName(filename);
  std::lock_guard<std::mutex> lock(builtins_mutex);
  for (BuiltinLibrary* library : Builtins()) {
    if (library->name == base) return library;
  }
  return nullptr;
}

BuiltinLibrary* AsBuiltin(void* handle) {
  std::lock_guard<std::mutex> lock(builtins_mutex);
  for (BuiltinLibrary* library : Builtins()) {
    if (library == handle) return library;
  }
  return nullptr;
}

// --- ELF objects (<gesher/elf_dlopen.h>) ------------------------------------

std::atomic<const gesher_elf_dlopen_ops*> elf_ops{nullptr};

// Handles the ELF loader returned, under builtins_mutex.
std::vector<void*>& ElfHandles() {
  static std::vector<void*> handles;
  return handles;
}

bool IsElfHandle(void* handle) {
  std::lock_guard<std::mutex> lock(builtins_mutex);
  for (void* h : ElfHandles()) {
    if (h == handle) return true;
  }
  return false;
}

// Whether the file at `path` starts with the ELF magic.
bool IsElfFile(const char* path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  unsigned char magic[4] = {};
  bool elf = read(fd, magic, sizeof(magic)) == 4 && magic[0] == 0x7f && magic[1] == 'E' &&
             magic[2] == 'L' && magic[3] == 'F';
  close(fd);
  return elf;
}

// GESHER_TRACE_DL (docs/compiled-code.md): what Windows said when dlerror()
// gives the Android linker's text instead.
bool TraceDl() {
  static const bool on = getenv("GESHER_TRACE_DL") != nullptr;
  return on;
}

// dlerror() for a path LoadLibraryExW could not load, as Android's linker
// words it (bionic linker.cpp load_library, linker_phdr.cpp ReadElfHeader and
// VerifyElfHeader) when the path names no file or a file that is not a
// library: an app that dlopen()s or System.load()s its own path sees what it
// would on a device. Empty when Windows' text stays: a bare name (looked up
// along the DLL search path), a PE image (a Windows DLL of Gesher's own,
// whose failure is a Windows one, a missing dependency say), or a file that
// cannot be classified.
std::string AndroidLoadError(const char* filename) {
  if (strpbrk(filename, "/\\") == nullptr) return std::string();
  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return std::string("dlopen failed: library \"") + filename + "\" not found";
    }
    return std::string();
  }
  struct stat st = {};
  // The size of an ELF64 header (Elf64_Ehdr), what the linker reads first.
  unsigned char header[64] = {};
  ssize_t got = -1;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
    got = 0;
    while (got < static_cast<ssize_t>(sizeof(header))) {
      ssize_t n = read(fd, header + got, sizeof(header) - got);
      if (n <= 0) break;
      got += n;
    }
  }
  close(fd);
  if (got < 0) return std::string();
  if (got >= 2 && header[0] == 'M' && header[1] == 'Z') return std::string();
  if (st.st_size == 0) {
    return std::string("dlopen failed: file offset for the library \"") + filename +
           "\" >= file size: 0 >= 0";
  }
  if (got < static_cast<ssize_t>(sizeof(header))) {
    return std::string("dlopen failed: \"") + filename +
           "\" is too small to be an ELF executable: only found " + std::to_string(got) +
           " bytes";
  }
  // A file that starts with the ELF magic went to the ELF loader before.
  // The path is not part of the formatted text: a long one is never cut short.
  char text[16];
  snprintf(text, sizeof(text), "%02x%02x%02x%02x", header[0], header[1], header[2], header[3]);
  return std::string("dlopen failed: \"") + filename + "\" has bad ELF magic: " + text;
}

void* OpenElf(const char* filename, int flags) {
  const gesher_elf_dlopen_ops* ops = elf_ops.load();
  if (ops == nullptr) {
    last_error = std::string("dlopen ") + filename + ": an ELF object, and no ELF loader";
    has_error = true;
    return nullptr;
  }
  char error[1024] = {};
  void* handle = ops->open(filename, flags, error, sizeof(error));
  if (handle == nullptr) {
    // Android's linker prefixes its dlerror() text so (as the bionic shim's
    // dlopen does for the same loader).
    last_error = std::string("dlopen failed: ") + error;
    has_error = true;
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(builtins_mutex);
  ElfHandles().push_back(handle);
  return handle;
}

}  // namespace

extern "C" {

void gesher_set_elf_dlopen(const gesher_elf_dlopen_ops* ops) { elf_ops.store(ops); }

void gesher_register_builtin_library(const char* name, const gesher_builtin_symbol* symbols,
                                     size_t count) {
  std::lock_guard<std::mutex> lock(builtins_mutex);
  Builtins().push_back(new BuiltinLibrary{name, symbols, count});
}

void* dlopen(const char* filename, int flags) {
  if (filename == nullptr) return GetModuleHandleW(nullptr);
  if (BuiltinLibrary* library = FindBuiltin(filename)) return library;
  if (IsElfFile(filename)) return OpenElf(filename, flags);
  std::wstring path = gesher::Widen(filename);
  DWORD load_flags = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
  const bool has_drive = path.size() >= 2 && path[1] == L':' &&
      ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'));
  if (path.find_first_of(L"/\\") != std::wstring::npos || has_drive) {
    // Consume a relative path once: another thread can change the process's
    // current directory between a size query and the subsequent conversion.
    // This bounds the allocation at Windows' maximum UTF-16 path length.
    std::vector<wchar_t> full_path(32768);
    const DWORD length = GetFullPathNameW(path.c_str(),
        static_cast<DWORD>(full_path.size()), full_path.data(), nullptr);
    if (length == 0 || length >= full_path.size()) {
      if (length != 0) SetLastError(ERROR_FILENAME_EXCED_RANGE);
      SetError("dlopen", filename);
      return nullptr;
    }
    path.assign(full_path.data(), length);
    load_flags |= LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
  }
  HMODULE module = nullptr;
  if ((flags & RTLD_NOLOAD) != 0) {
    // A successful dlopen owns the reference that its dlclose releases, even
    // when it is only looking up an already loaded module.
    GetModuleHandleExW(0, path.c_str(), &module);
  } else {
    module = LoadLibraryExW(path.c_str(), nullptr, load_flags);
  }
  if (module == nullptr) {
    const DWORD windows_error = GetLastError();
    std::string android = (flags & RTLD_NOLOAD) != 0 ? std::string() : AndroidLoadError(filename);
    if (android.empty()) {
      SetLastError(windows_error);
      SetError("dlopen", filename);
      // Safe bare-name lookup reports a real missing module, rather than the
      // invalid-parameter error from using DLL_LOAD_DIR without a full path.
      if (windows_error == ERROR_MOD_NOT_FOUND) last_error += ": library not found";
    } else {
      if (TraceDl()) {
        fprintf(stderr, "gesher dl: dlopen(\"%s\"): LoadLibraryExW: Windows error %lu\n", filename,
                windows_error);
      }
      last_error = std::move(android);
      has_error = true;
    }
  }
  return module;
}

void* dlsym(void* handle, const char* symbol) {
  if (BuiltinLibrary* library = AsBuiltin(handle)) {
    for (size_t i = 0; i < library->count; ++i) {
      if (strcmp(library->symbols[i].name, symbol) == 0) return library->symbols[i].address;
    }
    last_error = "dlsym " + std::string(symbol) + ": not in " + library->name;
    has_error = true;
    return nullptr;
  }
  if (IsElfHandle(handle)) {
    void* p = elf_ops.load()->sym(handle, symbol);
    if (p == nullptr) {
      last_error = "dlsym " + std::string(symbol) + ": not found in the ELF object";
      has_error = true;
    }
    return p;
  }
  if (handle != RTLD_DEFAULT && handle != RTLD_NEXT) {
    FARPROC p = GetProcAddress(static_cast<HMODULE>(handle), symbol);
    if (p == nullptr) SetError("dlsym", symbol);
    return reinterpret_cast<void*>(p);
  }
  // Search every loaded module, the executable first.
  std::vector<HMODULE> modules(256);
  DWORD needed = 0;
  HANDLE process = GetCurrentProcess();
  if (EnumProcessModules(process, modules.data(),
                         static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed) &&
      needed > modules.size() * sizeof(HMODULE)) {
    modules.resize(needed / sizeof(HMODULE));
    EnumProcessModules(process, modules.data(), needed, &needed);
  }
  modules.resize(needed / sizeof(HMODULE));
  for (HMODULE m : modules) {
    if (FARPROC p = GetProcAddress(m, symbol)) return reinterpret_cast<void*>(p);
  }
  SetError("dlsym", symbol);
  return nullptr;
}

int dlclose(void* handle) {
  if (handle == GetModuleHandleW(nullptr) || AsBuiltin(handle) != nullptr) return 0;
  if (IsElfHandle(handle)) return elf_ops.load()->close(handle);
  if (!FreeLibrary(static_cast<HMODULE>(handle))) {
    SetError("dlclose", nullptr);
    return -1;
  }
  return 0;
}

char* dlerror(void) {
  if (!has_error) return nullptr;
  has_error = false;
  return last_error.data();
}

int dladdr(const void* addr, Dl_info* info) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(addr), &module)) {
    return 0;
  }
  static thread_local std::string name;
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
  name = n > 0 ? gesher::Narrow(path) : std::string();
  info->dli_fname = name.c_str();
  info->dli_fbase = module;
  info->dli_sname = nullptr;  // PE files keep no symbol names for internal code
  info->dli_saddr = nullptr;
  return 1;
}

}  // extern "C"
