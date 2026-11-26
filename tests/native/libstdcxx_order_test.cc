// An app library that ships its own C++ runtime (libc++_shared.so, the NDK's
// default shared STL) keeps libc++'s operator new and __cxa_guard_* although
// the shim also implements bionic's libstdc++.so: loaded objects are searched
// before the shim, for relocations and for dlsym(RTLD_DEFAULT), while
// dlopen("libstdc++.so") still reaches the platform library. Run by
// elf_load_test.exe with libc++_shared.so in the search directory.

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <new>

extern "C" int __cxa_guard_acquire(long long*);

namespace {

int failures;

// The file name of the loaded object that defines `p`; the shim's functions
// belong to no loaded object.
const char* Where(void* p) {
  Dl_info info;
  if (p != nullptr && dladdr(p, &info) != 0 && info.dli_fname != nullptr) {
    const char* slash = strrchr(info.dli_fname, '/');
    return slash != nullptr ? slash + 1 : info.dli_fname;
  }
  return p != nullptr ? "(platform)" : "(null)";
}

void Expect(const char* label, const char* where, const char* expected) {
  bool ok = strcmp(where, expected) == 0;
  failures += ok ? 0 : 1;
  printf("%s libstdcxx_order: %s: %s\n", ok ? "ok  " : "FAIL", label, where);
}

int Counted() {
  static int value = printf("libstdcxx_order: function-local static constructed\n");
  return value;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int gesher_selftest() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  void* (*bound_new)(size_t) = &::operator new;
  Expect("bound operator new", Where(reinterpret_cast<void*>(bound_new)), "libc++_shared.so");
  Expect("bound __cxa_guard_acquire", Where(reinterpret_cast<void*>(&__cxa_guard_acquire)),
         "libc++_shared.so");
  Expect("dlsym(RTLD_DEFAULT, \"_Znwm\")", Where(dlsym(RTLD_DEFAULT, "_Znwm")), "libc++_shared.so");
  Expect("dlsym(RTLD_DEFAULT, \"__cxa_guard_acquire\")",
         Where(dlsym(RTLD_DEFAULT, "__cxa_guard_acquire")), "libc++_shared.so");
  void* platform = dlopen("libstdc++.so", RTLD_NOW);
  Expect("dlsym(libstdc++.so, \"_Znwm\")", Where(platform ? dlsym(platform, "_Znwm") : nullptr),
         "(platform)");
  failures += Counted() > 0 && Counted() > 0 ? 0 : 1;
  int* p = new int(5);
  failures += *p == 5 ? 0 : 1;
  delete p;
  if (failures == 0) printf("libstdcxx_order: all passed\n");
  else printf("libstdcxx_order: %d failed\n", failures);
  return failures;
}
