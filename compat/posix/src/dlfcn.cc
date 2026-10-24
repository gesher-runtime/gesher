// gesher-base: <dlfcn.h> over LoadLibrary / GetProcAddress for Windows DLLs.
// Android ELF libraries go to the ELF loader registered through
// <gesher/elf_dlopen.h> (phase 6).

#include <dlfcn.h>
#include <fcntl.h>
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
    last_error = std::string("dlopen ") + filename + ": " + error;
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
  HMODULE module = (flags & RTLD_NOLOAD) != 0
      ? GetModuleHandleW(path.c_str())
      : LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
  if (module == nullptr) SetError("dlopen", filename);
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
