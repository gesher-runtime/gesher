// Exercise the production engine with small per-call limits. The ordinary ABI
// always uses the documented defaults; no large input or memory pressure is used.
#include "posix_regex.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace gesher::bionic::posix_regex;
namespace {
int failures = 0, checks = 0;
std::atomic<long> outstanding{0};
std::atomic<bool> track{false};
void Check(const char* name, bool ok) {
  ++checks; failures += !ok;
  std::printf("regex budget: %s %s\n", name, ok ? "PASS" : "FAIL");
}
}
// Link wrapping observes actual arena allocation/deallocation, including unwind
// before upstream C cleanup labels, without adding counters to ordinary code.
extern "C" void* __real_malloc(size_t);
extern "C" void __real_free(void*);
extern "C" void* __wrap_malloc(size_t bytes) {
  void* value = __real_malloc(bytes);
  if (track && value != nullptr) ++outstanding;
  return value;
}
extern "C" void __wrap_free(void* value) {
  if (track && value != nullptr) --outstanding;
  __real_free(value);
}

int main() {
  for (Limits limit : {Limits{32, 50000, 256}, Limits{4096, 24, 256}, Limits{4096, 50000, 2}}) {
    int result;
    track = true;
    { Program program(false); result = Compile(program, "(((ab)))", nullptr, 1, 1, limit); }
    track = false;
    Check("partial compilation returns REG_ESPACE", result == kRegEspace);
    Check("partial compiler allocations retire", outstanding == 0);
  }
  {
    Program program(false);
    Check("compile backreference control", Compile(program, "\\([ab]\\)\\1", nullptr, 0, 1) == 0);
    netbsd::regmatch_t matches[2]{{71, 72}, {81, 82}};
    track = true;
    int result = Execute(program, "ababab", 2, matches, 0, 1, {4096, 128, 256});
    track = false;
    Check("prefilter consumes execution budget", result == kRegEspace);
    Check("prefilter failure leaves output untouched", matches[0].rm_so == 71 && matches[1].rm_so == 81);
    Check("prefilter failure retires scratch", outstanding == 0);
    Check("adequate budget distinguishes no match", Execute(program, "ababab", 2, matches, 0, 1) == kRegNomatch);
    Check("adequate budget preserves successful match", Execute(program, "aab", 2, matches, 0, 1) == 0 && matches[0].rm_eo == 2);
  }
  {
    Program program(false);
    Check("compile state and capture control", Compile(program, "((ab|a){3,6})", nullptr, 1, 1) == 0);
    netbsd::regmatch_t matches[3]{{71, 72}, {81, 82}, {91, 92}};
    track = true;
    int result = Execute(program, "abababab", 3, matches, 0, 1, {256, 50000, 256});
    track = false;
    Check("combined state and capture storage respects limit", result == kRegEspace);
    Check("allocation failure leaves captures untouched", matches[0].rm_so == 71 && matches[2].rm_so == 91);
    Check("allocation failure retires scratch", outstanding == 0);
    Check("ordinary state and capture control matches", Execute(program, "abababab", 3, matches, 0, 1) == 0 && matches[0].rm_eo == 8);
    track = true;
    result = Execute(program, "abababab", 3, matches, 0, 1, {4096, 50000, 1});
    track = false;
    Check("dissection recursion consumes depth budget", result == kRegEspace);
    Check("depth failure retires scratch", outstanding == 0);
  }
  {
    Program program(false);
    Check("compile output publication control", Compile(program, "a", nullptr, 1, 1) == 0);
    netbsd::regmatch_t matches[100];
    for (auto& match : matches) match = {71, 72};
    int result = Execute(program, "a", 100, matches, 0, 1, {4096, 100, 256});
    bool unchanged = true;
    for (auto& match : matches) unchanged &= match.rm_so == 71 && match.rm_eo == 72;
    Check("output work is charged before publication", result == kRegEspace && unchanged);
    Check("subsequent call is not poisoned", Execute(program, "a", 100, matches, 0, 1) == 0 && matches[0].rm_eo == 1 && matches[99].rm_so == -1);
  }
  {
    bool exhausted = false;
    track = true;
    {
      Arena arena(256);
      Context context(arena, {}, false, 1);
      Scope scope(context);
      void* first = Alloc(100);
      try { (void)ReallocArray<char>(first, 100, 1); } catch (const ResourceLimit&) { exhausted = true; }
      Check("reallocation limit counts old and new blocks", exhausted && arena.used() > 100 && arena.used() <= 256);
    }
    track = false;
    Check("failed realloc retains and then retires original allocation", outstanding == 0);
  }
  {
    Program program(false);
    Check("compile concurrent reusable expression", Compile(program, "\\(a\\)\\1", nullptr, 0, 1) == 0);
    std::atomic<int> errors{0};
    auto worker = [&](size_t budget) {
      for (int i = 0; i < 100; ++i) {
        netbsd::regmatch_t match{71, 72};
        int result = Execute(program, "aa", 1, &match, 0, 1, {4096, budget, 256});
        if (budget == 1 ? result != kRegEspace || match.rm_so != 71 : result != 0 || match.rm_eo != 2) ++errors;
      }
    };
    std::thread first(worker, 1), second(worker, 50000);
    first.join(); second.join();
    Check("compiled program and thread budgets are independent", errors == 0 && current == nullptr);
  }
  std::printf("regex budget: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
