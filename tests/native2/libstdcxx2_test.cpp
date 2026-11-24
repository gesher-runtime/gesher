// native2 track: Android's libstdc++.so (operator new/delete, std::nothrow,
// the static-initialisation guards, __cxa_pure_virtual) on the bionic shim,
// as an Android ELF executable run by gesher-exec
// (tests/windows/tracks/44-native2.ps1). Built with -nostdlib++ against the
// NDK's libstdc++.so stub, so every import below is DT_NEEDED libstdc++.so,
// as a platform library links (issue #480: Dreame's libemgmo.so got a null
// operator new). Without arguments prints "libstdcxx2_test: all passed".
// "new-fails" and "pure-virtual" end in bionic's abort(): the SIGABRT handler
// this program installs runs and prints
// "libstdcxx2_test: SIGABRT handler ran (<mode>)".
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <new>

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    printf("libstdcxx2_test: FAIL %s\n", what);
    ++g_failures;
  }
}

struct Node {
  int value;
  char text[24];
  virtual ~Node() = default;
  virtual int Twice() const { return value * 2; }
};

void Allocation() {
  Node* node = new Node();
  Check(node != nullptr, "new Node");
  node->value = 21;
  strcpy(node->text, "allocated");
  Check(node->Twice() == 42 && strcmp(node->text, "allocated") == 0, "virtual call on new Node");
  delete node;

  int* array = new int[1000];
  for (int i = 0; i < 1000; ++i) array[i] = i;
  Check(array[999] == 999, "new int[1000]");
  delete[] array;

  int* quiet = new (std::nothrow) int(7);
  Check(quiet != nullptr && *quiet == 7, "new (std::nothrow) int");
  operator delete(quiet, std::nothrow);
  char* quiet_array = new (std::nothrow) char[64];
  Check(quiet_array != nullptr, "new (std::nothrow) char[64]");
  operator delete[](quiet_array, std::nothrow);

  volatile size_t huge = SIZE_MAX / 2;
  Check(::operator new(huge, std::nothrow) == nullptr, "nothrow new of SIZE_MAX/2 returns null");
}

void Library() {
  void* handle = dlopen("libstdc++.so", RTLD_NOW);
  Check(handle != nullptr, "dlopen libstdc++.so");
  if (handle == nullptr) return;
  Check(dlsym(handle, "_ZSt7nothrow") == static_cast<const void*>(&std::nothrow),
        "dlsym std::nothrow is the object this program was bound to");
  Check(dlsym(handle, "_Znwm") != nullptr && dlsym(handle, "__cxa_guard_acquire") != nullptr,
        "dlsym operator new and __cxa_guard_acquire");
}

// A function-local static with a slow constructor, raced by 16 threads: one
// construction, and every thread sees the constructed value.
int g_constructions = 0;
struct Slow {
  int value;
  Slow() {
    __atomic_add_fetch(&g_constructions, 1, __ATOMIC_SEQ_CST);
    usleep(20000);
    value = 1234;
  }
};
__attribute__((noinline)) int SlowValue() {
  static Slow slow;
  return slow.value;
}
void* Racer(void*) { return reinterpret_cast<void*>(static_cast<intptr_t>(SlowValue())); }

void Guards() {
  pthread_t threads[16];
  for (pthread_t& t : threads) pthread_create(&t, nullptr, Racer, nullptr);
  bool all = true;
  for (pthread_t t : threads) {
    void* result = nullptr;
    pthread_join(t, &result);
    all &= reinterpret_cast<intptr_t>(result) == 1234;
  }
  Check(g_constructions == 1, "one construction of a raced function-local static");
  Check(all, "every racing thread sees the constructed static");
}

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
  static const char kMessage[] = "libstdcxx2_test: SIGABRT handler ran (";
  write(1, kMessage, sizeof(kMessage) - 1);
  write(1, g_mode, strlen(g_mode));
  write(1, ")\n", 2);
  _exit(0);
}
}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1) {
    g_mode = argv[1];
    signal(SIGABRT, OnAbort);
    if (strcmp(argv[1], "new-fails") == 0) {
      volatile size_t huge = SIZE_MAX / 2;
      void* p = ::operator new(huge);
      printf("libstdcxx2_test: FAIL operator new(SIZE_MAX/2) returned %p\n", p);
    } else if (strcmp(argv[1], "pure-virtual") == 0) {
      Derived d;
      printf("libstdcxx2_test: FAIL the pure virtual call returned\n");
    }
    return 1;
  }
  Allocation();
  Library();
  Guards();
  if (g_failures == 0) printf("libstdcxx2_test: all passed\n");
  return g_failures == 0 ? 0 : 1;
}
