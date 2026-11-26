// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 The Gesher Authors
//
// Link the complete production syscall_bionic.cc. The three unused raw-syscall
// boundaries below are inert; regcomp/regexec/regfree execute their real ABI.
#include "shim_internal.h"
#include "posix_regex.h"
#include <gesher/elf_loader.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace regex_work {
uint64_t popped = 0, expanded = 0, input = 0;
}

namespace gesher::elf {
void* MapLoadedMemory(void*, size_t, int, int, int, int64_t, bool) {
  return errno = ENOSYS, reinterpret_cast<void*>(-1);
}
int ProtectLoadedMemory(void*, size_t, int, bool) { return errno = ENOSYS, -1; }
}
namespace gesher::bionic {
void* LookupBionicSymbol(const char*) { return nullptr; }
}

struct Regex { int32_t magic; size_t count; const char* end; void* guts; };
struct Match { int64_t begin, end; };
using Compile = int (BIONIC_SYSV*)(Regex*, const char*, int);
using Execute = int (BIONIC_SYSV*)(const Regex*, const char*, size_t, Match*, int);
using Free = void (BIONIC_SYSV*)(Regex*);

#ifdef REGEX_ANDROID_REFERENCE
extern "C" int android_regcomp(Regex*, const char*, int);
extern "C" int android_regexec(const Regex*, const char*, size_t, Match*, int);
extern "C" void android_regfree(Regex*);
#endif

namespace {
Compile compile;
Execute execute;
Free free_regex;
uint64_t checks, failures, fingerprint = 1469598103934665603ULL;

void Check(const char* label, bool ok) {
  ++checks;
  if (!ok) {
    ++failures;
    if (failures <= 12) std::printf("FAIL %s\n", label);
  }
}

void Hash(uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    fingerprint ^= static_cast<uint8_t>(value >> (8 * i));
    fingerprint *= 1099511628211ULL;
  }
}

uint64_t Random(uint64_t& seed) {
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  return seed;
}

struct Pattern {
  const char* text;
  int flags;
};

// Ordinary C-locale BRE/ERE patterns, including the dissection and backreference
// choices for which glibc is not the Android oracle.
constexpr Pattern patterns[] = {
    {"a", 1}, {"a*", 1}, {"a+", 1}, {"a?", 1}, {"a|aa", 1},
    {"(a|aa)*", 1}, {"(aa|a)*", 1}, {"(a*)*", 1}, {"(a(b)|a)+", 1},
    {"((a|b)*)c", 1}, {"a*(a|aa)b?", 1}, {"^a*$", 1},
    {"^(a?){4}a{4}$", 1}, {"((ab)?a)*", 1}, {"(a|b)(a|aa)*", 1},
    {"([ab]*)(a*)(b?)", 1}, {"(a|ab)(b|bc)", 1}, {"(foo|foobar)(bar)?", 1},
    {"[[:alpha:]]+", 1}, {"[[:space:]]", 1}, {"[^a]", 1}, {"^$", 1},
    {"a$", 1}, {"^a", 1}, {"a|ba*", 1}, {"a.*", 1}, {"a.b", 1},
    {"^a[[:space:]]b$", 1}, {"[[:<:]]a[[:>:]]", 1}, {"(a?){3}b", 1},
    {"\\(a\\)\\1", 0}, {"a*\\(b\\)\\1", 0},
    {"\\(a*\\)\\{1,2\\}\\1", 0}, {"\\(a*\\)*\\1", 0},
    {"\\(a\\)\\{1,2\\}b", 0}, {"\\<\\(a\\)\\1", 0},
};

void Differential() {
  uint64_t seed = 0xfedcba987654321ULL;
  uint64_t cases = 0;
  constexpr char alphabet[] = {'a', 'b', 'A', 'c', '\n', '\0', '_', ' '};
  for (const auto& pattern : patterns) {
    for (int extra : {0, 2, 8, 2 | 8, 4, 4 | 8}) {
      Regex re{};
      int flags = pattern.flags | extra;
      const int compiled = compile(&re, pattern.text, flags);
      Check("curated pattern compiles", compiled == 0);
#ifdef REGEX_ANDROID_REFERENCE
      Regex oracle{};
      const int reference_compiled = android_regcomp(&oracle, pattern.text, flags);
      Check("Android compilation agrees", reference_compiled == compiled);
#endif
      if (compiled != 0) continue;
      for (unsigned sample = 0; sample < 384; ++sample) {
        ++cases;
        std::string input(Random(seed) % 13, 'a');
        for (char& c : input) c = alphabet[Random(seed) % sizeof(alphabet)];
        int eflags = static_cast<int>(Random(seed) % 4);
        const bool bounded = (Random(seed) % 4) != 0;
        if (bounded) eflags |= 4;
        const size_t nmatch = std::array<size_t, 5>{0, 1, 2, 4, 8}[Random(seed) % 5];
        std::array<Match, 8> actual;
        for (auto& m : actual) m = {-77, -88};
        if (bounded) {
          const size_t start = Random(seed) % (input.size() + 1);
          actual[0] = {static_cast<int64_t>(start),
                       static_cast<int64_t>(start + Random(seed) % (input.size() - start + 1))};
        }
        const auto before = actual;
        const int rc = execute(&re, input.c_str(), nmatch, actual.data(), eflags);
        Check("short input is match or nonmatch", rc == 0 || rc == 1);
        if ((flags & 4) != 0 || nmatch == 0)
          Check("boolean query preserves every caller slot",
                std::memcmp(before.data(), actual.data(), sizeof(actual)) == 0);
        Hash(static_cast<uint64_t>(rc));
        if (rc == 0 && (flags & 4) == 0)
          for (size_t i = 0; i < nmatch; ++i) {
            Hash(static_cast<uint64_t>(actual[i].begin));
            Hash(static_cast<uint64_t>(actual[i].end));
          }
#ifdef REGEX_ANDROID_REFERENCE
        auto expected = before;
        const int reference_rc = android_regexec(&oracle, input.c_str(), nmatch,
                                                 expected.data(), eflags);
        bool same = reference_rc == rc;
        if (same && rc == 0 && (flags & 4) == 0)
          same = std::memcmp(actual.data(), expected.data(), nmatch * sizeof(Match)) == 0;
        if (!same && failures < 12)
          std::printf("ORACLE pattern=%s cflags=%d eflags=%d nmatch=%zu sample=%u\n",
                      pattern.text, flags, eflags, nmatch, sample);
        Check("Android result and observable captures agree", same);
#endif
      }
      free_regex(&re);
#ifdef REGEX_ANDROID_REFERENCE
      if (reference_compiled == 0) android_regfree(&oracle);
#endif
    }
  }
  std::printf("REGEX_DIFFERENTIAL cases=%llu fingerprint=%llu\n",
              static_cast<unsigned long long>(cases), static_cast<unsigned long long>(fingerprint));
}

void GuardPage() {
#if !defined(_WIN32)
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto* memory = static_cast<char*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check("guard mapping", memory != MAP_FAILED);
  if (memory == MAP_FAILED) return;
  Check("guard protection", mprotect(memory + page, page, PROT_NONE) == 0);
  char* input = memory + page - 6;
  std::memcpy(input, "xaaabb", 6);
  for (int cflags : {1, 1 | 4}) {
    Regex re{};
    Check("guard pattern compiles", compile(&re, "(a?){3}b+", cflags) == 0);
    for (size_t count : {size_t{0}, size_t{1}, size_t{3}}) {
      Match matches[3] = {{1, 6}, {-77, -88}, {-77, -88}};
      Check("bounded NFA does not read terminator or past end",
            execute(&re, input, count, matches, 4) == 0);
      if (cflags == 1 && count != 0)
        Check("bounded NFA keeps longest observable span",
              matches[0].begin == 1 && matches[0].end == 6);
      else
        Check("bounded boolean preserves region",
              matches[0].begin == 1 && matches[0].end == 6);
      matches[0] = {3, 2};
      Check("boolean still validates reversed region",
            execute(&re, input, count, matches, 4) == 16);
      Check("boolean still validates absent region",
            execute(&re, input, count, nullptr, 4) == 16);
    }
    free_regex(&re);
  }
  munmap(memory, page * 2);
  // A legal virtual range with no accessible bytes proves the existing input
  // size limit is checked before a nullable boolean expression can succeed.
  const size_t oversized = static_cast<size_t>(std::numeric_limits<int32_t>::max()) + 1;
  auto* inaccessible = static_cast<char*>(mmap(nullptr, oversized, PROT_NONE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check("oversized inaccessible range maps without committing input pages",
        inaccessible != MAP_FAILED);
  if (inaccessible != MAP_FAILED) {
    for (int cflags : {1, 1 | 4}) {
      Regex re{};
      Check("nullable resource-bound pattern compiles", compile(&re, "a*", cflags) == 0);
      for (size_t count : {size_t{0}, size_t{1}}) {
        Match range{0, static_cast<int64_t>(oversized)};
        Check("boolean query retains existing REG_ESPACE input bound before any read",
              execute(&re, inaccessible, count, &range, 4) == 12);
      }
      free_regex(&re);
    }
    munmap(inaccessible, oversized);
  }
#endif
}

struct Workload {
  const char* name;
  std::string pattern, input;
  int cflags;
  size_t nmatch;
  int result;
  int64_t start = -1, end = -1;
  bool single_start = false;
};

std::vector<Workload> Workloads() {
  std::string big = "a" + std::string(256 * 1024, 'x');
  return {
      {"nullable-hit", "^(a?){128}a{128}$", std::string(128, 'a'), 1, 1, 0, 0, 128, true},
      {"nullable-miss", "^(a?){128}b$", std::string(128, 'a') + "c", 1, 1, 1, -1, -1, true},
      {"nullable-search", "(a?){32}a{32}b", std::string(256, 'a') + "b", 1, 1, 0, 192, 257},
      {"boolean-zero-count", "a.*", big, 1, 0, 0},
      {"boolean-nosub", "a.*", big, 1 | 4, 8, 0},
      {"observable-long-span", "a.*", big, 1, 1, 0, 0, static_cast<int64_t>(big.size())},
      {"ordinary-hit", "^[a-z]+@[a-z]+\\.[a-z]+$", "name@example.com", 1, 1, 0, 0, 16, true},
      {"ordinary-miss", "^[a-z]+@[a-z]+\\.[a-z]+$", "name.example.com", 1, 1, 1, -1, -1, true},
      {"literal-search", "needle", std::string(4096, 'x') + "needle", 1, 1, 0, 4096, 4102},
      {"captures", "([a-z]+):([0-9]+)", "prefix key:12345 suffix", 1, 4, 0, 7, 16},
      {"repeated-captures", "(a|aa)*", std::string(128, 'a'), 1, 2, 0, 0, 128},
      {"backreference", "\\(ab\\)\\1", "xxababyy", 0, 2, 0, 2, 6},
  };
}

uint64_t Run(const Workload& w, const Regex& re, uint64_t loops) {
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < loops; ++i) {
    Match m[8] = {};
    m[0] = {0, static_cast<int64_t>(w.input.size())};
    const int rc = execute(&re, w.input.data(), w.nmatch, m, 4);
    checksum += static_cast<uint64_t>(rc + m[0].begin + m[0].end);
  }
  return checksum;
}

void Work(bool measure) {
  for (const auto& w : Workloads()) {
    Regex re{};
    Check("workload compiles", compile(&re, w.pattern.c_str(), w.cflags) == 0);
    if (re.guts == nullptr) continue;
    Match m[8] = {};
    m[0] = {0, static_cast<int64_t>(w.input.size())};
    regex_work::popped = regex_work::expanded = regex_work::input = 0;
    const int rc = execute(&re, w.input.data(), w.nmatch, m, 4);
    Check(w.name, rc == w.result);
    if (rc == 0 && w.nmatch != 0 && (w.cflags & 4) == 0)
      Check("workload whole-match span", m[0].begin == w.start && m[0].end == w.end);
    const auto* program = static_cast<const gesher::bionic::posix_regex::Program*>(re.guts);
    const size_t insns = program->insns.size();
#ifdef REGEX_WORK_BOUNDS
    if (w.single_start)
      Check("one origin expands each NFA state at most once per input position",
            regex_work::expanded <= insns * (w.input.size() + 1));
    if (w.nmatch == 0 || (w.cflags & 4) != 0)
      Check("boolean a-dot-star stops after its first accepted input byte",
            regex_work::input == 1 && regex_work::expanded <= 2 * insns);
#endif
    std::printf("REGEX_WORK {\"name\":\"%s\",\"instructions\":%zu,\"bytes\":%zu,"
                "\"popped\":%llu,\"expanded\":%llu,\"input\":%llu}\n", w.name, insns,
                w.input.size(), static_cast<unsigned long long>(regex_work::popped),
                static_cast<unsigned long long>(regex_work::expanded),
                static_cast<unsigned long long>(regex_work::input));
    if (measure) {
      uint64_t iterations = 1, checksum = 0;
      double seconds = 0;
      do {
        const std::clock_t start = std::clock();
        checksum = Run(w, re, iterations);
        seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
        if (seconds >= 0.08 || iterations >= (1u << 22)) break;
        iterations *= seconds > 0.02 ? 2 : 4;
      } while (true);
      std::printf("REGEX_TIME {\"name\":\"%s\",\"iterations\":%llu,\"ns_per_call\":%.3f,"
                  "\"checksum\":%llu}\n", w.name, static_cast<unsigned long long>(iterations),
                  seconds * 1e9 / static_cast<double>(iterations),
                  static_cast<unsigned long long>(checksum));
    }
    free_regex(&re);
  }
}
}

int main(int argc, char** argv) {
  gesher::bionic::SymbolTable symbols;
  gesher::bionic::RegisterSyscall(symbols);
  compile = reinterpret_cast<Compile>(symbols.at("regcomp"));
  execute = reinterpret_cast<Execute>(symbols.at("regexec"));
  free_regex = reinterpret_cast<Free>(symbols.at("regfree"));
  const std::string mode = argc > 1 ? argv[1] : "--check";
  if (mode == "--check") {
    Differential();
    GuardPage();
  } else {
    Work(mode == "--measure");
  }
  std::printf("regex scaling: %llu checks, %llu failures\n",
              static_cast<unsigned long long>(checks), static_cast<unsigned long long>(failures));
  return failures != 0;
}
