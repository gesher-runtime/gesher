// An x86-64 Android .so in C++ against the NDK's libc++_shared.so, which the
// ELF loader loads as its DT_NEEDED dependency from the same directory.
// C++ exceptions are thrown and caught inside the loaded code: libc++abi's
// __cxa_throw, LLVM libunwind (inside libc++_shared) walking .eh_frame found
// through dl_iterate_phdr, the personality routine and the LSDA, cleanups on
// the way, exceptions thrown by libc++ itself, in another thread, and
// exception_ptr. elf_load_test calls gesher_selftest(); it returns the number
// of failed checks. Built with the stack protector at API 29 (ADR 0014).

#include <stdio.h>
#include <string.h>

#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

namespace {

int failures;

void check(const char* what, bool ok) {
  printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
  fflush(stdout);
  if (!ok) ++failures;
}

struct Custom : std::exception {
  explicit Custom(int c) : code(c) {}
  const char* what() const noexcept override { return "custom"; }
  int code;
};

int g_destroyed;
struct Counter {
  ~Counter() { ++g_destroyed; }
};

__attribute__((noinline)) void deep(int n) {
  Counter c;
  std::string s(static_cast<size_t>(n) + 20, 'x');  // a heap string to clean up
  if (n == 0) throw Custom(42);
  deep(n - 1);
  printf("%s\n", s.c_str());  // not reached
}

struct Base {
  virtual ~Base() = default;
};
struct Derived : Base {
  int value = 7;
};

__attribute__((noinline)) Base* make(bool derived) {
  return derived ? new Derived() : new Base();
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int gesher_selftest() {
  failures = 0;
  printf("libcxx_test: C++ exceptions through libc++_shared\n");

  try {
    throw std::runtime_error("boom");
  } catch (const std::runtime_error& e) {
    check("throw and catch std::runtime_error", strcmp(e.what(), "boom") == 0);
  }

  bool caught = false;
  try {
    std::vector<int> v(3);
    (void)v.at(10);  // libc++ throws std::out_of_range
  } catch (const std::out_of_range&) {
    caught = true;
  }
  check("std::out_of_range thrown by libc++ itself", caught);

  g_destroyed = 0;
  int code = 0;
  try {
    deep(5);
  } catch (const std::exception& e) {
    const Custom* c = dynamic_cast<const Custom*>(&e);
    code = c != nullptr ? c->code : -1;
  }
  check("through 6 frames, caught as a base class", code == 42);
  check("cleanups ran on the way (6 destructors)", g_destroyed == 6);

  int rethrown = 0;
  try {
    try {
      throw 17;
    } catch (...) {
      rethrown = 1;
      throw;
    }
  } catch (int v) {
    rethrown = rethrown == 1 && v == 17 ? 2 : -1;
  }
  check("catch (...) and rethrow", rethrown == 2);

  std::exception_ptr ep;
  try {
    throw std::logic_error("kept");
  } catch (...) {
    ep = std::current_exception();
  }
  std::string kept;
  try {
    std::rethrow_exception(ep);
  } catch (const std::logic_error& e) {
    kept = e.what();
  }
  check("exception_ptr and rethrow_exception", kept == "kept");

  caught = false;
  try {
    (void)std::stoi("not a number");
  } catch (const std::invalid_argument&) {
    caught = true;
  }
  check("std::stoi throws std::invalid_argument", caught);

  std::unique_ptr<Base> b(make(true));
  Derived* d = dynamic_cast<Derived*>(b.get());
  check("dynamic_cast and typeid", d != nullptr && d->value == 7 &&
                                       typeid(*b) == typeid(Derived));
  bool bad_cast = false;
  try {
    std::unique_ptr<Base> plain(make(false));
    (void)dynamic_cast<Derived&>(*plain);
  } catch (const std::bad_cast&) {
    bad_cast = true;
  }
  check("std::bad_cast from a failed reference cast", bad_cast);

  // Each thread has its own exception state (__cxa_get_globals).
  std::mutex m;
  int thread_ok = 0;
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([i, &m, &thread_ok] {
      for (int k = 0; k < 50; ++k) {
        try {
          throw Custom(i * 100 + k);
        } catch (const Custom& e) {
          if (e.code == i * 100 + k && k == 49) {
            std::lock_guard<std::mutex> lock(m);
            ++thread_ok;
          }
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  check("4 std::threads throw and catch 50 times each", thread_ok == 4);

  std::string s = "abc";
  for (int i = 0; i < 10; ++i) s += std::to_string(i);
  check("std::string and std::to_string", s == "abc0123456789");

  if (failures == 0) printf("libcxx_test: all passed\n");
  fflush(stdout);
  return failures;
}
