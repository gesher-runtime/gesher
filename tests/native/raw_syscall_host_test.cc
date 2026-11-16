// Production syscall_bionic.cc is linked unchanged. Controlled libc boundaries
// verify argument adaptation without depending on a Windows scheduler/radio.
#include "shim_internal.h"
#include <gesher/elf_loader.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures;
void Check(const char* name, bool ok) {
  std::printf("raw syscall contracts: %s %s\n", name, ok ? "PASS" : "FAIL");
  failures += !ok;
}
int event_flags = -1, chmod_flags = -1, mask_calls;
int64_t BIONIC_SYSV Eventfd(int64_t initial, int64_t flags, int64_t, int64_t, int64_t, int64_t) {
  event_flags = static_cast<int>(flags);
  return flags < 0 ? (errno = EINVAL, -1) : initial + 100;
}
int64_t BIONIC_SYSV Fchmodat(int64_t, int64_t, int64_t, int64_t flags, int64_t, int64_t) {
  chmod_flags = static_cast<int>(flags);
  return 0;
}
int64_t BIONIC_SYSV Close(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) {
  errno = EBADF;
  return -1;
}
int64_t BIONIC_SYSV Getpid(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) { return 123; }
int64_t BIONIC_SYSV Getaffinity(int64_t, int64_t size, int64_t set, int64_t, int64_t, int64_t) {
  if (size < 16) return errno = EINVAL, -1;
  std::memset(reinterpret_cast<void*>(set), 0, static_cast<size_t>(size));
  reinterpret_cast<unsigned char*>(set)[8] = 1;  // 65 configured CPUs
  return 0;
}
int64_t BIONIC_SYSV Sysconf(int name) { return name == 0x60 ? 65 : -1; }
int64_t BIONIC_SYSV Sigprocmask(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) {
  ++mask_calls;
  return 0;
}
struct Regex { int32_t magic; size_t count; const char* end; void* guts; };
struct Match { int64_t begin, end; };
}

namespace gesher::elf {
void* MapLoadedMemory(void*, size_t, int, int, int, int64_t, bool) { errno = ENOSYS; return (void*)-1; }
int ProtectLoadedMemory(void*, size_t, int, bool) { errno = ENOSYS; return -1; }
}

namespace gesher::bionic {
void* LookupBionicSymbol(const char* name) {
  if (!std::strcmp(name, "eventfd")) return reinterpret_cast<void*>(&Eventfd);
  if (!std::strcmp(name, "fchmodat")) return reinterpret_cast<void*>(&Fchmodat);
  if (!std::strcmp(name, "close")) return reinterpret_cast<void*>(&Close);
  if (!std::strcmp(name, "getpid")) return reinterpret_cast<void*>(&Getpid);
  if (!std::strcmp(name, "sched_getaffinity")) return reinterpret_cast<void*>(&Getaffinity);
  if (!std::strcmp(name, "sysconf")) return reinterpret_cast<void*>(&Sysconf);
  if (!std::strcmp(name, "sigprocmask")) return reinterpret_cast<void*>(&Sigprocmask);
  return nullptr;
}
}

int main() {
  using namespace gesher::bionic;
  int64_t args[6] = {7, -1, 0, -1, 0, 0};
  errno = EDOM;
  Check("old eventfd ignores unused register", RawSyscallX64(284, args) == 107 && event_flags == 0);
  Check("raw success keeps errno", errno == EDOM);
  errno = EDOM;
  Check("eventfd2 retains flags", RawSyscallX64(290, args) == -EINVAL && event_flags == -1);
  Check("raw failure keeps errno", errno == EDOM);
  Check("old fchmodat supplies zero flags", RawSyscallX64(268, args) == 0 && chmod_flags == 0);
  Check("rt_sigprocmask rejects wrong sigset size", RawSyscallX64(14, args) == -EINVAL && mask_calls == 0);
  args[3] = 8;
  Check("rt_sigprocmask accepts kernel sigset size", RawSyscallX64(14, args) == 0 && mask_calls == 1);
  unsigned char mask[32];
  args[1] = sizeof(mask);
  args[2] = reinterpret_cast<int64_t>(mask);
  Check("affinity returns actual mask byte count", RawSyscallX64(204, args) == 16 && mask[8] == 1);
  args[1] = 8;
  Check("affinity errors stay negative errno", RawSyscallX64(204, args) == -EINVAL);
  Check("unknown syscall remains ENOSYS", RawSyscallX64(500, args) == -ENOSYS);
  Check("int error result remains EBADF", RawSyscallX64(3, args) == -EBADF);

  SymbolTable symbols;
  RegisterSyscall(symbols);
  auto compile = reinterpret_cast<int (BIONIC_SYSV*)(Regex*, const char*, int)>(symbols.at("regcomp"));
  auto execute = reinterpret_cast<int (BIONIC_SYSV*)(const Regex*, const char*, size_t, Match*, int)>(symbols.at("regexec"));
  auto free_regex = reinterpret_cast<void (BIONIC_SYSV*)(Regex*)>(symbols.at("regfree"));
  Regex re{};
  Check("compile extended regex", compile(&re, "[0-9]+", 1) == 0);
  Match match{1, 3};
  const char bounded[] = {'x', '4', '2', 'y'};
  Check("bounded non-terminated input", execute(&re, bounded, 1, &match, 4) == 0 &&
                                          match.begin == 1 && match.end == 3);
  match = {3, 1};
  Check("inverted range rejected", execute(&re, "x42y", 1, &match, 4) == 16);
  match = {-1, 3};
  Check("negative range rejected", execute(&re, "x42y", 1, &match, 4) == 16);
  free_regex(&re);
  Check("REG_NEWLINE matches per-line anchors",
        compile(&re, "^x$", 1 | 8) == 0 && execute(&re, "a\nx\nb", 0, nullptr, 0) == 0 &&
            execute(&re, "a\nx\nb", 0, nullptr, 1 | 2) == 0);
  free_regex(&re);
  Check("REG_NEWLINE matches trailing empty line and respects offsets",
        compile(&re, "^$", 1 | 8) == 0 &&
            execute(&re, "a\n", 1, &match, 0) == 0 && match.begin == 2 && match.end == 2);
  free_regex(&re);
  Check("REG_NEWLINE excludes newline from dot and negated class",
        compile(&re, "a.b", 1 | 8) == 0 && execute(&re, "a\nb", 0, nullptr, 0) != 0 &&
            execute(&re, "axb", 0, nullptr, 0) == 0);
  free_regex(&re);
  Check("REG_NEWLINE excludes newline from negated bracket expressions",
        compile(&re, "a[^x]b", 1 | 8) == 0 && execute(&re, "a\nb", 0, nullptr, 0) != 0 &&
            execute(&re, "ayb", 0, nullptr, 0) == 0);
  free_regex(&re);
  Check("REG_NEWLINE negated bracket with trailing dash still excludes newline",
        compile(&re, "a[^a-]b", 1 | 8) == 0 && execute(&re, "a\nb", 0, nullptr, 0) != 0 &&
            execute(&re, "a-b", 0, nullptr, 0) != 0 && execute(&re, "axb", 0, nullptr, 0) == 0);
  free_regex(&re);
  Check("REG_NEWLINE positive character classes still match newline",
        compile(&re, "a[[:space:]]b", 1 | 8) == 0 &&
            ((match = {0, 0}), execute(&re, "a\nb", 1, &match, 0) == 0) &&
            match.begin == 0 && match.end == 3);
  free_regex(&re);
  Check("REG_NEWLINE positive cntrl class matches newline",
        compile(&re, "a[[:cntrl:]]b", 1 | 8) == 0 &&
            ((match = {0, 0}), execute(&re, "a\nb", 1, &match, 0) == 0) &&
            match.begin == 0 && match.end == 3);
  free_regex(&re);
  Check("REG_NEWLINE anchored multiline positive class matches",
        compile(&re, "^a[[:space:]]b$", 1 | 8) == 0 &&
            ((match = {0, 0}), execute(&re, "z\na\nb\nw", 1, &match, 0) == 0) &&
            match.begin == 2 && match.end == 5);
  free_regex(&re);
  Check("REG_NEWLINE with REG_STARTEND keeps offsets relative to base",
        compile(&re, "^cd$", 1 | 8) == 0 &&
            ((match = {1, 6}), execute(&re, "ab\ncd\n", 1, &match, 4) == 0) &&
            match.begin == 3 && match.end == 5);
  free_regex(&re);
  Check("REG_NEWLINE with REG_STARTEND and REG_NOTBOL honors preceding newline",
        compile(&re, "^x$", 1 | 8) == 0 &&
            ((match = {2, 3}), execute(&re, "a\nx", 1, &match, 1 | 4) == 0) &&
            match.begin == 2 && match.end == 3);
  free_regex(&re);
  Check("REG_NEWLINE with REG_NOTBOL without preceding newline rejects start",
        compile(&re, "^x$", 1 | 8) == 0 && execute(&re, "x\ny", 0, nullptr, 1) != 0);
  free_regex(&re);
  Check("REG_NEWLINE extended handles escaped ordinary slash and escaped bracket",
        compile(&re, "^\\|[0-9]{2}\\/[0-9]{5}\\^$", 1 | 8) == 0 &&
            execute(&re, "|11/11111^", 0, nullptr, 0) == 0 &&
            execute(&re, "|11x11111^", 0, nullptr, 0) != 0);
  free_regex(&re);
  Check("escaped bracket stays literal outside character class",
        compile(&re, "a\\[b\\/c\\]", 1) == 0 && execute(&re, "a[b/c]", 0, nullptr, 0) == 0 &&
            execute(&re, "ab", 0, nullptr, 0) != 0);
  free_regex(&re);
  Check("REG_NEWLINE matches literal newline in pattern",
        compile(&re, "a\nb", 1 | 8) == 0 && execute(&re, "a\nb", 0, nullptr, 0) == 0);
  free_regex(&re);
  for (const char* pat : {"a$\nb", "a\n^b", "^a$\n^b$", "^a\\\nb$"}) {
    match = {0, 0};
    Check("REG_NEWLINE internal newline anchor/escaped LF matches",
          compile(&re, pat, 1 | 8) == 0 && execute(&re, "a\nb", 1, &match, 0) == 0 &&
              match.begin == 0 && match.end == 3);
    free_regex(&re);
  }
  Match sub[3]{};
  Check("POSIX leftmost-longest captures (a|ab)(b|bc)",
        compile(&re, "(a|ab)(b|bc)", 1) == 0 && execute(&re, "abc", 3, sub, 0) == 0 &&
            sub[0].begin == 0 && sub[0].end == 3 && sub[1].begin == 0 && sub[1].end == 1 &&
            sub[2].begin == 1 && sub[2].end == 3);
  free_regex(&re);
  Check("POSIX leftmost-longest captures (foo|foobar)(bar|)",
        compile(&re, "(foo|foobar)(bar|)", 1) == 0 && execute(&re, "foobar", 3, sub, 0) == 0 &&
            sub[0].begin == 0 && sub[0].end == 6 && sub[1].begin == 0 && sub[1].end == 6 &&
            sub[2].begin == 6 && sub[2].end == 6);
  free_regex(&re);
  Check("BRE backreference matches repeated group",
        compile(&re, "\\(ab\\)\\1", 0) == 0 && execute(&re, "xxababzz", 2, sub, 0) == 0 &&
            sub[0].begin == 2 && sub[0].end == 6 && sub[1].begin == 2 && sub[1].end == 4);
  free_regex(&re);
  {
    Regex re_space{};
    Regex re_alpha{};
    bool compiled = compile(&re_space, "^a[[:space:]]b$", 1 | 8) == 0 &&
                    compile(&re_alpha, "^[[:alpha:]]+$", 1 | 8) == 0;
    bool scaling_ok = compiled;
    for (int lines : {100, 200, 400, 800}) {
      std::string in_x, in_d;
      in_x.reserve(static_cast<size_t>(lines) * 2);
      in_d.reserve(static_cast<size_t>(lines) * 4);
      for (int i = 0; i < lines; ++i) {
        in_x += "x\n";
        in_d += "123\n";
      }
      auto t0 = std::chrono::steady_clock::now();
      bool miss = execute(&re_space, in_x.c_str(), 1, &match, 0) != 0 &&
                  execute(&re_alpha, in_d.c_str(), 1, &match, 0) != 0;
      auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
      scaling_ok = scaling_ok && miss && dt_ms < 500;
    }
    Check("REG_NEWLINE multiline scaling stays bounded", scaling_ok);
    if (compiled) {
      free_regex(&re_space);
      free_regex(&re_alpha);
    }
  }
  Check("REG_PEND needs an end pointer", compile(&re, "x", 32) == 16);
  std::printf("raw syscall contracts: %d failures\n", failures);
  return failures != 0;
}
