// dlopen()'s dlerror() text (compat/posix/src/dlfcn.cc), the one ART puts in
// System.load()'s UnsatisfiedLinkError: for a path that names no file, or a
// file that is not a library, Android's linker wording; for Windows DLLs
// (Gesher's own), Windows' diagnostics.
#include <dlfcn.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
int checks, failures;
void Check(const char* name, bool ok, const char* detail) {
  ++checks;
  if (!ok) ++failures;
  std::printf("dlopen-errors: %s %s", ok ? "PASS" : "FAIL", name);
  if (!ok) std::printf(" (dlerror: %s)", detail != nullptr ? detail : "(null)");
  std::printf("\n");
}
std::string Error() {
  const char* e = dlerror();
  return e != nullptr ? e : std::string();
}
bool Write(const std::string& path, const void* data, size_t size) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = std::fwrite(data, 1, size, f) == size;
  return std::fclose(f) == 0 && ok;
}
}  // namespace

int main() {
  char temp[MAX_PATH];
  const DWORD n = GetTempPathA(MAX_PATH, temp);
  if (n == 0 || n >= MAX_PATH) return 2;
  std::string dir = std::string(temp) + "gesher-dlopen-errors-" + std::to_string(GetCurrentProcessId());
  for (char& c : dir) {
    if (c == '\\') c = '/';
  }
  CreateDirectoryA(dir.c_str(), nullptr);

  // No such file: an app's own path, as a packer extracts and loads it.
  const std::string missing = dir + "/.jiagu/libmissing.so";
  void* h = dlopen(missing.c_str(), RTLD_NOW);
  std::string e = Error();
  Check("a missing path is \"library ... not found\"",
        h == nullptr && e == "dlopen failed: library \"" + missing + "\" not found", e.c_str());

  const std::string empty = dir + "/libempty.so";
  Write(empty, "", 0);
  h = dlopen(empty.c_str(), RTLD_NOW);
  e = Error();
  Check("an empty file is past its end, as the linker says",
        h == nullptr &&
            e == "dlopen failed: file offset for the library \"" + empty + "\" >= file size: 0 >= 0",
        e.c_str());

  const std::string small = dir + "/libsmall.so";
  Write(small, "not a lib\n", 10);
  h = dlopen(small.c_str(), RTLD_NOW);
  e = Error();
  Check("a short file is too small to be an ELF executable",
        h == nullptr && e == "dlopen failed: \"" + small +
                                 "\" is too small to be an ELF executable: only found 10 bytes",
        e.c_str());

  const std::string text = dir + "/libtext.so";
  std::string body(200, 'x');
  body.replace(0, 5, "hello");
  Write(text, body.data(), body.size());
  h = dlopen(text.c_str(), RTLD_NOW);
  e = Error();
  Check("another file has bad ELF magic",
        h == nullptr && e == "dlopen failed: \"" + text + "\" has bad ELF magic: 68656c6c",
        e.c_str());

  // A PE image keeps Windows' diagnostics: a Windows DLL Gesher loads.
  const std::string pe = dir + "/broken.dll";
  std::string image(256, '\0');
  image[0] = 'M';
  image[1] = 'Z';
  Write(pe, image.data(), image.size());
  h = dlopen(pe.c_str(), RTLD_NOW);
  e = Error();
  Check("a PE image that fails keeps the Windows error",
        h == nullptr && e.find("Windows error") != std::string::npos, e.c_str());

  // A bare name is a Windows DLL lookup: Windows' text.
  h = dlopen("gesher-no-such-library-4f2a.dll", RTLD_NOW);
  e = Error();
  Check("a missing bare name keeps the Windows error",
        h == nullptr && e.find("Windows error") != std::string::npos, e.c_str());

  // A Windows DLL by path still loads.
  char system[MAX_PATH];
  GetSystemDirectoryA(system, MAX_PATH);
  const std::string version = std::string(system) + "\\version.dll";
  h = dlopen(version.c_str(), RTLD_NOW);
  Check("a Windows DLL by path loads", h != nullptr && dlsym(h, "GetFileVersionInfoW") != nullptr,
        dlerror());
  if (h != nullptr) dlclose(h);

  DeleteFileA(empty.c_str());
  DeleteFileA(small.c_str());
  DeleteFileA(text.c_str());
  DeleteFileA(pe.c_str());
  RemoveDirectoryA(dir.c_str());
  std::printf("dlopen-errors: %d assertions, %d failures\n", checks, failures);
  return failures != 0;
}
