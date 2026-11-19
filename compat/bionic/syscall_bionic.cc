// gesher bionic shim: raw x86-64 Linux system calls, ptrace and POSIX regex.
//
// Raw system calls (ADR 0084). Packers' stubs and anti-tamper code make Linux
// system calls with the `syscall` instruction instead of calling libc (UPX's
// unpacker maps and protects memory that way; Ijiami's and Jiagu's code reads
// /proc and calls ptrace so). The ELF loader turns each such instruction into
// a trap and hands the call here: the number selects the shim function that
// does the same (the x86-64 table, arch/x86/entry/syscalls/syscall_64.tbl),
// called with the kernel's arguments, and its -1/errno result becomes the
// kernel's -errno. syscall(2) uses the same table for numbers it has no case
// of its own for. A call with no shim function returns -ENOSYS, as an old
// kernel would.
//
// ptrace: nothing traces a Gesher process and it traces nothing. The first
// PTRACE_TRACEME succeeds, as on a device, and a second one then fails with
// EPERM (the process is "traced" by its parent, as anti-debugging checks
// expect); attaching to or peeking at another process fails with EPERM, and
// any other request with ESRCH (no such tracee).
//
// regcomp/regexec/regerror/regfree: bionic's (BSD) regex ABI over
// posix_regex::Program (POSIX BRE/ERE with leftmost-longest whole-match selection,
// NetBSD/Android REG_NEWLINE anchor/bracket rules, and bounded NFA matching).

#include "shim_internal.h"

#include "posix_regex.h"
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <errno.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gesher::bionic {
namespace {

// --- raw system calls ------------------------------------------------------------

// How a shim function returns: 'i' an int, -1 and errno on failure; 'l' a
// long (ssize_t, off_t), -1 and errno; 'p' a pointer, MAP_FAILED (-1) and errno;
// 'n' a pointer, nullptr and errno (getcwd: the kernel returns the length);
// 'e' an int that is the error number itself (clock_nanosleep).
struct RawCall {
  uint16_t nr;
  const char* fn;
  char ret;
};
constexpr RawCall kRawCalls[] = {
    {0, "read", 'l'},          {1, "write", 'l'},          {2, "open", 'i'},
    {3, "close", 'i'},         {4, "stat", 'i'},           {5, "fstat", 'i'},
    {6, "lstat", 'i'},         {7, "poll", 'i'},           {8, "lseek", 'l'},
    {9, "mmap", 'p'},          {10, "mprotect", 'i'},      {11, "munmap", 'i'},
    {14, "sigprocmask", 'i'},  {16, "ioctl", 'i'},         {17, "pread64", 'l'},
    {18, "pwrite64", 'l'},     {19, "readv", 'l'},         {20, "writev", 'l'},
    {21, "access", 'i'},       {22, "pipe", 'i'},          {24, "sched_yield", 'i'},
    {25, "mremap", 'p'},       {26, "msync", 'i'},         {27, "mincore", 'i'},
    {28, "madvise", 'i'},      {32, "dup", 'i'},           {33, "dup2", 'i'},
    {35, "nanosleep", 'i'},    {39, "getpid", 'i'},        {40, "sendfile", 'l'},
    {41, "socket", 'i'},       {42, "connect", 'i'},       {43, "accept", 'i'},
    {44, "sendto", 'l'},       {45, "recvfrom", 'l'},      {46, "sendmsg", 'l'},
    {47, "recvmsg", 'l'},      {48, "shutdown", 'i'},      {49, "bind", 'i'},
    {50, "listen", 'i'},       {51, "getsockname", 'i'},   {52, "getpeername", 'i'},
    {53, "socketpair", 'i'},   {54, "setsockopt", 'i'},    {55, "getsockopt", 'i'},
    {60, "_exit", 'i'},        {61, "wait4", 'i'},         {62, "kill", 'i'},
    {63, "uname", 'i'},        {72, "fcntl", 'i'},         {73, "flock", 'i'},
    {74, "fsync", 'i'},        {75, "fdatasync", 'i'},     {76, "truncate", 'i'},
    {77, "ftruncate", 'i'},    {79, "getcwd", 'n'},        {80, "chdir", 'i'},
    {81, "fchdir", 'i'},       {82, "rename", 'i'},        {83, "mkdir", 'i'},
    {84, "rmdir", 'i'},        {86, "link", 'i'},          {87, "unlink", 'i'},
    {88, "symlink", 'i'},      {89, "readlink", 'l'},      {90, "chmod", 'i'},
    {91, "fchmod", 'i'},       {95, "umask", 'i'},         {96, "gettimeofday", 'i'},
    {97, "getrlimit", 'i'},    {98, "getrusage", 'i'},     {99, "sysinfo", 'i'},
    {100, "times", 'l'},       {101, "ptrace", 'l'},       {102, "getuid", 'i'},
    {104, "getgid", 'i'},      {107, "geteuid", 'i'},      {108, "getegid", 'i'},
    {110, "getppid", 'i'},     {111, "getpgrp", 'i'},      {112, "setsid", 'i'},
    {137, "statfs", 'i'},      {138, "fstatfs", 'i'},      {149, "mlock", 'i'},
    {150, "munlock", 'i'},     {151, "mlockall", 'i'},    {152, "munlockall", 'i'},
    {157, "prctl", 'i'},
    {186, "gettid", 'i'},      {200, "tkill", 'i'},        {201, "time", 'l'},
    {204, "sched_getaffinity", 'i'}, {228, "clock_gettime", 'i'}, {229, "clock_getres", 'i'},
    {230, "clock_nanosleep", 'e'}, {231, "_exit", 'i'},    {232, "epoll_wait", 'i'},
    {233, "epoll_ctl", 'i'},   {234, "tgkill", 'i'},       {254, "inotify_add_watch", 'i'},
    {255, "inotify_rm_watch", 'i'}, {257, "openat", 'i'},  {258, "mkdirat", 'i'},
    {262, "fstatat", 'i'},     {263, "unlinkat", 'i'},     {264, "renameat", 'i'},
    {265, "linkat", 'i'},      {266, "symlinkat", 'i'},    {267, "readlinkat", 'l'},
    {268, "fchmodat", 'i'},    {269, "faccessat", 'i'},    {271, "ppoll", 'i'},
    {281, "epoll_pwait", 'i'}, {284, "eventfd", 'i'},      {288, "accept4", 'i'},
    {290, "eventfd", 'i'},     {291, "epoll_create1", 'i'}, {292, "dup3", 'i'},
    {293, "pipe2", 'i'},       {294, "inotify_init1", 'i'}, {318, "getrandom", 'l'},
    {319, "memfd_create", 'i'}, {325, "mlock2", 'i'},
};

using ShimFn6 = int64_t (BIONIC_SYSV*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

struct ResolvedCall {
  ShimFn6 fn = nullptr;
  char ret = 0;
};
constexpr size_t kMaxNr = 326;

// The shim functions, looked up once (the table is complete by the time
// loaded code runs).
const ResolvedCall* Calls() {
  static const ResolvedCall* table = [] {
    auto* t = new ResolvedCall[kMaxNr]();
    for (const RawCall& c : kRawCalls) {
      t[c.nr] = {reinterpret_cast<ShimFn6>(LookupBionicSymbol(c.fn)), c.ret};
    }
    return t;
  }();
  return table;
}

// The kernel's x86-64 struct sigaction, and bionic's LP64 one (signal_bionic.cc).
struct KernelSigaction {
  uint64_t handler;
  uint64_t flags;
  uint64_t restorer;
  uint64_t mask;
};
struct ShimSigaction {
  int32_t flags;
  int32_t pad;
  uint64_t handler;
  uint64_t mask;
  uint64_t restorer;
};

int64_t RtSigaction(const int64_t a[6]) {
  using SigactionFn = int (BIONIC_SYSV*)(int, const ShimSigaction*, ShimSigaction*);
  static auto fn = reinterpret_cast<SigactionFn>(LookupBionicSymbol("sigaction"));
  if (fn == nullptr) return -kENOSYS;
  if (a[3] != 8) return -kEINVAL;  // sigsetsize
  const auto* in = reinterpret_cast<const KernelSigaction*>(a[1]);
  auto* out = reinterpret_cast<KernelSigaction*>(a[2]);
  ShimSigaction act{}, old{};
  if (in != nullptr) {
    act.flags = static_cast<int32_t>(in->flags);
    act.handler = in->handler;
    act.mask = in->mask;
    act.restorer = in->restorer;
  }
  if (fn(static_cast<int>(a[0]), in != nullptr ? &act : nullptr, out != nullptr ? &old : nullptr) != 0) {
    return -errno;
  }
  if (out != nullptr) {
    out->handler = old.handler;
    out->flags = static_cast<uint32_t>(old.flags);
    out->restorer = old.restorer;
    out->mask = old.mask;
  }
  return 0;
}

// --- ptrace ------------------------------------------------------------------------

std::atomic<bool> g_traceme{false};

int64_t BIONIC_SYSV Ptrace(int request, int pid, void* addr, void* data) {
  (void)pid;
  (void)addr;
  (void)data;
  constexpr int kTraceme = 0, kPeektext = 1, kPeekdata = 2, kPeekuser = 3, kAttach = 16,
                kSeize = 0x4206;
  switch (request) {
    case kTraceme:
      if (g_traceme.exchange(true)) return errno = kEPERM, -1;
      return 0;
    case kAttach:
    case kSeize:
    case kPeektext:
    case kPeekdata:
    case kPeekuser:
      return errno = kEPERM, -1;
    default:
      return errno = kESRCH, -1;
  }
}

// --- POSIX regex (bionic's BSD ABI) ------------------------------------------------

struct BionicRegex {
  int32_t re_magic;
  size_t re_nsub;
  const char* re_endp;
  void* re_g;
};
static_assert(sizeof(BionicRegex) == 32);
struct BionicRegmatch {
  int64_t rm_so;
  int64_t rm_eo;
};

constexpr int32_t kRegMagic = 0xf265;

using posix_regex::Compiler;
using posix_regex::kRegBadpat;
using posix_regex::kRegEspace;
using posix_regex::kRegExtended;
using posix_regex::kRegIcase;
using posix_regex::kRegInvarg;
using posix_regex::kRegNewline;
using posix_regex::kRegNomatch;
using posix_regex::kRegNosub;
using posix_regex::kRegNotbol;
using posix_regex::kRegNoteol;
using posix_regex::kRegOk;
using posix_regex::kRegPend;
using posix_regex::kRegStartend;
using posix_regex::Matcher;
using posix_regex::Program;

int BIONIC_SYSV Regcomp(BionicRegex* preg, const char* pattern, int cflags) {
  if (preg == nullptr || pattern == nullptr) return kRegInvarg;
  constexpr int supported = kRegExtended | kRegIcase | kRegNosub | kRegNewline | kRegPend;
  if ((cflags & ~supported) != 0 ||
      ((cflags & kRegPend) != 0 &&
       (preg->re_endp == nullptr || reinterpret_cast<uintptr_t>(preg->re_endp) <
                                        reinterpret_cast<uintptr_t>(pattern)))) {
    return kRegInvarg;
  }
  const bool extended = (cflags & kRegExtended) != 0;
  const bool icase = (cflags & kRegIcase) != 0;
  const bool nosub = (cflags & kRegNosub) != 0;
  const bool newline = (cflags & kRegNewline) != 0;
  try {
    std::string raw = (cflags & kRegPend) != 0 && preg->re_endp != nullptr
                          ? std::string(pattern, preg->re_endp)
                          : std::string(pattern);
    auto compiled = std::make_unique<Program>();
    Compiler compiler(raw, extended, icase, nosub, newline);
    int err = compiler.Compile(*compiled);
    if (err != kRegOk) {
      return err;
    }
    preg->re_magic = kRegMagic;
    preg->re_nsub = compiled->nsub;
    preg->re_g = compiled.release();
    return 0;
  } catch (...) {
    return kRegEspace;
  }
}

int BIONIC_SYSV Regexec(const BionicRegex* preg, const char* string, size_t nmatch,
                        BionicRegmatch* pmatch, int eflags) {
  if (preg == nullptr || preg->re_magic != kRegMagic || preg->re_g == nullptr) return kRegBadpat;
  if (string == nullptr) return kRegInvarg;
  const auto* compiled = static_cast<const Program*>(preg->re_g);
  const char* begin = string;
  const char* end;
  if ((eflags & kRegStartend) != 0) {
    // REG_STARTEND accepts a bounded byte sequence, including embedded NULs;
    // it must not first scan for a terminator outside that sequence.
    if (pmatch == nullptr || pmatch[0].rm_so < 0 || pmatch[0].rm_eo < pmatch[0].rm_so) {
      return kRegInvarg;
    }
    begin = string + pmatch[0].rm_so;
    end = string + pmatch[0].rm_eo;
  } else {
    end = string + std::strlen(string);
  }
  try {
    Matcher matcher(*compiled, string, begin, end, eflags, nmatch);
    std::vector<int32_t> caps;
    int32_t match_start = 0;
    int32_t match_end = 0;
    int rc = matcher.Execute(caps, match_start, match_end);
    if (rc != kRegOk) return rc;
    if (!compiled->nosub && pmatch != nullptr && nmatch > 0) {
      const int64_t base_offset = static_cast<int64_t>(begin - string);
      pmatch[0].rm_so = base_offset + match_start;
      pmatch[0].rm_eo = base_offset + match_end;
      for (size_t i = 1; i < nmatch; ++i) {
        if (i <= compiled->nsub && 2 * i + 1 < caps.size() && caps[2 * i] >= 0) {
          pmatch[i].rm_so = base_offset + caps[2 * i];
          pmatch[i].rm_eo = base_offset + caps[2 * i + 1];
        } else {
          pmatch[i].rm_so = pmatch[i].rm_eo = -1;
        }
      }
    }
    return 0;
  } catch (...) {
    return kRegEspace;
  }
}

size_t BIONIC_SYSV Regerror(int errcode, const BionicRegex*, char* errbuf, size_t errbuf_size) {
  static const char* const kMessages[] = {
      "success",                          "regexec() failed to match",
      "invalid regular expression",       "invalid collating element",
      "invalid character class",          "trailing backslash (\\)",
      "invalid backreference number",     "brackets ([ ]) not balanced",
      "parentheses not balanced",         "braces not balanced",
      "invalid repetition count(s)",      "invalid character range",
      "out of memory",                    "repetition-operator operand invalid",
      "empty (sub)expression",            "cannot happen -- you found a bug",
      "invalid argument to regex routine", "illegal byte sequence"};
  const int code = errcode & 0xff;
  const char* message = code >= 0 && code < static_cast<int>(sizeof(kMessages) / sizeof(kMessages[0]))
                            ? kMessages[code]
                            : "unknown regexp error";
  const size_t length = std::strlen(message) + 1;
  if (errbuf != nullptr && errbuf_size > 0) {
    const size_t n = length < errbuf_size ? length : errbuf_size;
    std::memcpy(errbuf, message, n - 1);
    errbuf[n - 1] = '\0';
  }
  return length;
}

void BIONIC_SYSV Regfree(BionicRegex* preg) {
  if (preg == nullptr || preg->re_magic != kRegMagic) return;
  delete static_cast<Program*>(preg->re_g);
  preg->re_g = nullptr;
  preg->re_magic = 0;
}

}  // namespace

int64_t RawSyscallX64(int64_t nr, const int64_t a[6]) {
  // A raw kernel call returns -errno without modifying libc's thread-local
  // errno. syscall(2)'s caller sets errno only after a negative result.
  struct PreserveErrno {
    int saved = errno;
    ~PreserveErrno() { errno = saved; }
  } preserve_errno;
  if (nr == 13) return RtSigaction(a);  // rt_sigaction: the kernel's layout
  if (nr == 14 && a[3] != 8) return -kEINVAL;  // rt_sigprocmask sigsetsize
  // mmap/mprotect: code they make executable anywhere waits for its first
  // execution, when the loader rewrites it (an unpacker's own code).
  if (nr == 9) {
    errno = 0;
    void* p = gesher::elf::MapLoadedMemory(reinterpret_cast<void*>(a[0]), static_cast<size_t>(a[1]),
                                           static_cast<int>(a[2]), static_cast<int>(a[3]),
                                           static_cast<int>(a[4]), a[5], true);
    return p == reinterpret_cast<void*>(-1) ? -(errno != 0 ? errno : kEINVAL)
                                            : reinterpret_cast<int64_t>(p);
  }
  if (nr == 10) {
    errno = 0;
    return gesher::elf::ProtectLoadedMemory(reinterpret_cast<void*>(a[0]),
                                            static_cast<size_t>(a[1]), static_cast<int>(a[2]),
                                            true) == 0
               ? 0
               : -(errno != 0 ? errno : kEINVAL);
  }
  if (nr == 202) {                      // futex: syscall(2)'s own case
    using SyscallFn = int64_t (BIONIC_SYSV*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                                             int64_t, int64_t);
    static auto fn = reinterpret_cast<SyscallFn>(LookupBionicSymbol("syscall"));
    if (fn == nullptr) return -kENOSYS;
    int64_t r = fn(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
    return r == -1 ? -errno : r;
  }
  if (nr < 0 || static_cast<uint64_t>(nr) >= kMaxNr) return -kENOSYS;
  const ResolvedCall& call = Calls()[nr];
  if (call.fn == nullptr) return -kENOSYS;
  errno = 0;
  // The older kernel interfaces have fewer arguments than libc's wrappers.
  // Registers beyond the kernel signature are unspecified, not extra flags.
  const int64_t r = call.fn(a[0], nr == 284 ? 0 : a[1], a[2],
                           nr == 268 || nr == 269 ? 0 : a[3], a[4], a[5]);
  const int error = errno != 0 ? errno : kEINVAL;
  if (nr == 204 && static_cast<int32_t>(r) == 0) {
    // libc sched_getaffinity returns zero; the raw syscall returns the bytes
    // in the CPU mask. Use the same configured CPU count as the host shim.
    using SysconfFn = int64_t (BIONIC_SYSV*)(int);
    static auto sysconf_fn = reinterpret_cast<SysconfFn>(LookupBionicSymbol("sysconf"));
    if (sysconf_fn == nullptr) return -kENOSYS;
    const int64_t count = sysconf_fn(0x60);  // bionic _SC_NPROCESSORS_CONF
    return ((count > 0 ? count : 1) + 63) / 64 * 8;
  }
  switch (call.ret) {
    case 'i': {
      const auto v = static_cast<int32_t>(r);
      return v == -1 ? -error : v;
    }
    case 'n':
      if (r == 0) return -error;
      return static_cast<int64_t>(std::strlen(reinterpret_cast<const char*>(r)) + 1);
    case 'e': {
      const auto v = static_cast<int32_t>(r);
      return v > 0 ? -v : v;
    }
    default:  // 'l', 'p'
      return r == -1 ? -error : r;
  }
}

void RegisterSyscall(SymbolTable& t) {
  t["ptrace"] = reinterpret_cast<void*>(&Ptrace);
  t["regcomp"] = reinterpret_cast<void*>(&Regcomp);
  t["regexec"] = reinterpret_cast<void*>(&Regexec);
  t["regerror"] = reinterpret_cast<void*>(&Regerror);
  t["regfree"] = reinterpret_cast<void*>(&Regfree);
}

}  // namespace gesher::bionic
