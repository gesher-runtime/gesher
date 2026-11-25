// native2 track: the fatal paths of Android's libstdc++.so on the bionic shim,
// as an Android ELF executable run by gesher-exec
// (tests/windows/tracks/44-native2.ps1). Built with -nostdlib++ against the
// NDK's libstdc++.so stub, so operator new and the pure-virtual vtable slot
// bind to libstdc++.so through its DT_NEEDED entry (tests/native/libstdcxx_test.cc
// covers the non-fatal exports). As bionic's async_safe_fatal, a failed
// operator new and a pure virtual call end in abort(), which raises SIGABRT:
// the handler this program installs (as a crash reporter does) runs and prints
// "libstdcxx_fatal_test: SIGABRT handler ran (<mode>)".
//
// Usage: libstdcxx_fatal_test new-fails | pure-virtual
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <new>

namespace {
// A pure virtual call: Base's constructor calls f() through its own vtable.
struct Base {
  Base();
  virtual ~Base() = default;
  virtual void f() = 0;
};
__attribute__((noinline)) void CallF(Base* b) { b->f(); }
Base::Base() { CallF(this); }
struct Derived : Base {
  void f() override {}
};

const char* g_mode = "";
void OnAbort(int) {
  static const char kMessage[] = "libstdcxx_fatal_test: SIGABRT handler ran (";
  write(1, kMessage, sizeof(kMessage) - 1);
  write(1, g_mode, strlen(g_mode));
  write(1, ")\n", 2);
  _exit(0);
}
}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    printf("usage: libstdcxx_fatal_test new-fails | pure-virtual\n");
    return 2;
  }
  g_mode = argv[1];
  signal(SIGABRT, OnAbort);
  if (strcmp(argv[1], "new-fails") == 0) {
    volatile size_t huge = SIZE_MAX / 2;
    void* p = ::operator new(huge);
    printf("libstdcxx_fatal_test: FAIL operator new(SIZE_MAX/2) returned %p\n", p);
  } else if (strcmp(argv[1], "pure-virtual") == 0) {
    Derived d;
    printf("libstdcxx_fatal_test: FAIL the pure virtual call returned\n");
  } else {
    printf("libstdcxx_fatal_test: unknown mode %s\n", argv[1]);
  }
  return 1;
}
