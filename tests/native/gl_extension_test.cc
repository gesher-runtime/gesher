// gesher: runs a GL test library's gesher_selftest() with Gesher's EGL/GLES
// (compat/bionic/egl_gles.cc over ANGLE) in the bionic shim.
//
//   gl_extension_test <library.so>
//
// tests/native/gl_extension_procs_test.c checks that eglGetProcAddress and
// dlsym return the entry points of every GL extension the context advertises.
// Prints "gl_extension_test all passed" and exits 0 when the library reports
// no failure.
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <cstdio>
#include <string>

#if defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))
#else
#define SYSV
#endif

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: gl_extension_test <library.so>\n");
    return 2;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  gesher::bionic::InstallBionicShim();
  gesher::elf::SetupThreadTls();
  std::string error;
  gesher::elf::Library* lib = gesher::elf::Load(argv[1], &error);
  if (lib == nullptr) {
    std::printf("FAIL: load %s: %s\n", argv[1], error.c_str());
    return 1;
  }
  auto selftest = reinterpret_cast<int (SYSV*)()>(gesher::elf::Lookup(lib, "gesher_selftest"));
  if (selftest == nullptr) {
    std::printf("FAIL: %s exports no gesher_selftest\n", argv[1]);
    return 1;
  }
  const int failures = selftest();
  std::fflush(stdout);
  if (failures != 0) {
    std::printf("\n%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("\ngl_extension_test all passed\n");
  return 0;
}
