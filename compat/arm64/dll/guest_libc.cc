// gesher arm64 translator: guest-side libc pieces (gesher_arm64.dll; ADR 0047,
// docs/arm64.md).
//
// The guest calls the same bionic shim (compat/bionic) an x86-64 .so calls,
// through trampolines. That is right for nearly all of libc, because bionic's
// LP64 types are the same on x86-64 and arm64 — except for a few interfaces
// whose layout or numbering is architecture-specific:
//  - struct stat (144 bytes on x86-64, 128 on arm64) and struct epoll_event
//    (packed, 12 bytes on x86-64; 16 on arm64): wrappers convert;
//  - syscall(2) numbers (arm64 uses the generic table): a translation table
//    onto the shim's functions, shared with raw `SVC #0` in guest code;
//  - getauxval(AT_HWCAP): arm64 capability bits, only those the translator
//    implements (no LSE atomics, so outline-atomics code takes LL/SC paths);
//  - setjmp/longjmp: they save and restore registers, so they must be guest
//    code (AArch64, emitted here), not the host's;
//  - dlsym: returns these definitions for those names;
//  - the shim's va_list functions (vsnprintf...): registered so the guest's
//    AAPCS64 va_list is converted to System V when the guest calls them.
// The loader asks guest_symbol(name) before binding a guest import to the shim.

#include "translator.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace gesher_arm64 {

namespace {

using HostFn6 = int64_t(GESHER_SYSV*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

std::unordered_map<std::string, void*> g_overrides;

void* Host(const char* name) {
  return g_host != nullptr && g_host->resolve != nullptr ? g_host->resolve(name) : nullptr;
}

int* HostErrno() {
  static auto errno_fn = reinterpret_cast<int*(GESHER_SYSV*)()>(Host("__errno"));
  return errno_fn != nullptr ? errno_fn() : nullptr;
}

void SetErrno(int e) {
  if (int* p = HostErrno()) *p = e;
}

constexpr int kENOSYS = 38;
constexpr int kEFAULT = 14;

// --- struct stat ---------------------------------------------------------------
struct X64Stat {  // bionic x86-64 struct stat
  uint64_t st_dev, st_ino, st_nlink;
  uint32_t st_mode, st_uid, st_gid, pad0;
  uint64_t st_rdev;
  int64_t st_size, st_blksize, st_blocks;
  int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
  int64_t unused[3];
};
static_assert(sizeof(X64Stat) == 144, "x86-64 struct stat");
struct A64Stat {  // bionic arm64 struct stat
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, pad1;
  int64_t st_size;
  int32_t st_blksize, pad2;
  int64_t st_blocks;
  int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
  uint32_t unused4, unused5;
};
static_assert(sizeof(A64Stat) == 128, "arm64 struct stat");

void ToGuest(const X64Stat& h, A64Stat* g) {
  std::memset(g, 0, sizeof(*g));
  g->st_dev = h.st_dev;
  g->st_ino = h.st_ino;
  g->st_mode = h.st_mode;
  g->st_nlink = static_cast<uint32_t>(h.st_nlink);
  g->st_uid = h.st_uid;
  g->st_gid = h.st_gid;
  g->st_rdev = h.st_rdev;
  g->st_size = h.st_size;
  g->st_blksize = static_cast<int32_t>(h.st_blksize);
  g->st_blocks = h.st_blocks;
  g->atime = h.atime;
  g->atime_ns = h.atime_ns;
  g->mtime = h.mtime;
  g->mtime_ns = h.mtime_ns;
  g->ctime = h.ctime;
  g->ctime_ns = h.ctime_ns;
}

void* g_stat = nullptr;
void* g_lstat = nullptr;
void* g_fstat = nullptr;
void* g_fstatat = nullptr;

int GESHER_SYSV GuestStat(const char* path, A64Stat* st) {
  X64Stat h;
  int r = reinterpret_cast<int(GESHER_SYSV*)(const char*, X64Stat*)>(g_stat)(path, &h);
  if (r == 0 && st != nullptr) ToGuest(h, st);
  return r;
}
int GESHER_SYSV GuestLstat(const char* path, A64Stat* st) {
  X64Stat h;
  int r = reinterpret_cast<int(GESHER_SYSV*)(const char*, X64Stat*)>(g_lstat)(path, &h);
  if (r == 0 && st != nullptr) ToGuest(h, st);
  return r;
}
int GESHER_SYSV GuestFstat(int fd, A64Stat* st) {
  X64Stat h;
  int r = reinterpret_cast<int(GESHER_SYSV*)(int, X64Stat*)>(g_fstat)(fd, &h);
  if (r == 0 && st != nullptr) ToGuest(h, st);
  return r;
}
int GESHER_SYSV GuestFstatat(int dirfd, const char* path, A64Stat* st, int flags) {
  X64Stat h;
  int r = reinterpret_cast<int(GESHER_SYSV*)(int, const char*, X64Stat*, int)>(g_fstatat)(
      dirfd, path, &h, flags);
  if (r == 0 && st != nullptr) ToGuest(h, st);
  return r;
}

// --- struct epoll_event ------------------------------------------------------
struct __attribute__((packed)) X64EpollEvent {
  uint32_t events;
  uint64_t data;
};
struct A64EpollEvent {
  uint32_t events;
  uint32_t pad;
  uint64_t data;
};
static_assert(sizeof(X64EpollEvent) == 12 && sizeof(A64EpollEvent) == 16, "epoll_event");

void* g_epoll_ctl = nullptr;
void* g_epoll_wait = nullptr;
void* g_epoll_pwait = nullptr;

int GESHER_SYSV GuestEpollCtl(int ep, int op, int fd, A64EpollEvent* ev) {
  X64EpollEvent h{};
  if (ev != nullptr) {
    h.events = ev->events;
    h.data = ev->data;
  }
  return reinterpret_cast<int(GESHER_SYSV*)(int, int, int, X64EpollEvent*)>(g_epoll_ctl)(
      ep, op, fd, ev != nullptr ? &h : nullptr);
}
int GESHER_SYSV GuestEpollPwait(int ep, A64EpollEvent* evs, int max, int timeout,
                                const void* sigmask) {
  if (max <= 0 || evs == nullptr) {
    SetErrno(22);  // EINVAL
    return -1;
  }
  std::vector<X64EpollEvent> h(static_cast<size_t>(max));
  int r = g_epoll_pwait != nullptr
              ? reinterpret_cast<int(GESHER_SYSV*)(int, X64EpollEvent*, int, int, const void*)>(
                    g_epoll_pwait)(ep, h.data(), max, timeout, sigmask)
              : reinterpret_cast<int(GESHER_SYSV*)(int, X64EpollEvent*, int, int)>(g_epoll_wait)(
                    ep, h.data(), max, timeout);
  for (int i = 0; i < r; ++i) {
    evs[i].events = h[i].events;
    evs[i].pad = 0;
    evs[i].data = h[i].data;
  }
  return r;
}
int GESHER_SYSV GuestEpollWait(int ep, A64EpollEvent* evs, int max, int timeout) {
  return GuestEpollPwait(ep, evs, max, timeout, nullptr);
}

// --- getauxval -----------------------------------------------------------------
void* g_getauxval = nullptr;
// HWCAP_FP | ASIMD | AES | PMULL | SHA1 | SHA2 | CRC32: what dynarmic's A64
// frontend implements. Not HWCAP_ATOMICS (LSE is not implemented).
constexpr uint64_t kHwcap = 0xFB;
const char kPlatform[] = "aarch64";

uint64_t GESHER_SYSV GuestGetauxval(uint64_t type) {
  switch (type) {
    case 16: return kHwcap;                                // AT_HWCAP
    case 26: return 0;                                     // AT_HWCAP2
    case 15: return reinterpret_cast<uint64_t>(kPlatform);  // AT_PLATFORM
    default:
      return g_getauxval != nullptr
                 ? reinterpret_cast<uint64_t(GESHER_SYSV*)(uint64_t)>(g_getauxval)(type)
                 : 0;
  }
}

// --- syscall(2) ------------------------------------------------------------------
// arm64 (generic) syscall number -> the shim function that does the same,
// with how its result is returned. 'i': int (-1/errno), 'l': long/pointer.
struct SyscallMap {
  uint32_t nr;
  const char* fn;
  char ret;
};
const SyscallMap kSyscalls[] = {
    {17, "getcwd", 'p'},      {23, "dup", 'i'},          {24, "dup3", 'i'},
    {25, "fcntl", 'i'},       {29, "ioctl", 'i'},        {34, "mkdirat", 'i'},
    {35, "unlinkat", 'i'},    {38, "renameat", 'i'},     {46, "ftruncate", 'i'},
    {48, "faccessat", 'i'},   {56, "openat", 'i'},       {57, "close", 'i'},
    {59, "pipe2", 'i'},       {62, "lseek", 'l'},        {63, "read", 'l'},
    {64, "write", 'l'},       {65, "readv", 'l'},        {66, "writev", 'l'},
    {67, "pread64", 'l'},     {68, "pwrite64", 'l'},     {78, "readlinkat", 'l'},
    {82, "fsync", 'i'},       {83, "fdatasync", 'i'},    {94, "_exit", 'i'},
    {101, "nanosleep", 'i'},  {113, "clock_gettime", 'i'}, {114, "clock_getres", 'i'},
    {124, "sched_yield", 'i'}, {123, "sched_getaffinity", 'i'}, {129, "kill", 'i'},
    {131, "tgkill", 'i'},     {160, "uname", 'i'},       {163, "getrlimit", 'i'},
    {165, "getrusage", 'i'},  {167, "prctl", 'i'},       {169, "gettimeofday", 'i'},
    {172, "getpid", 'i'},     {173, "getppid", 'i'},     {174, "getuid", 'i'},
    {175, "geteuid", 'i'},    {176, "getgid", 'i'},      {177, "getegid", 'i'},
    {179, "sysinfo", 'i'},    {198, "socket", 'i'},      {203, "connect", 'i'},
    {215, "munmap", 'i'},     {222, "mmap", 'l'},        {226, "mprotect", 'i'},
    {233, "madvise", 'i'},    {278, "getrandom", 'l'},   {279, "memfd_create", 'i'},
};
std::unordered_map<uint32_t, std::pair<void*, char>> g_syscalls;
void* g_host_syscall = nullptr;

// libc-style result: -1 with errno set on failure.
int64_t DoSyscall(uint64_t nr, const int64_t a[6]) {
  switch (nr) {
    case 98:   // futex -> the shim's syscall(SYS_futex = 202)
    case 178:  // gettid -> SYS_gettid = 186
      if (g_host_syscall == nullptr) break;
      return reinterpret_cast<int64_t(GESHER_SYSV*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                                                     int64_t, int64_t)>(g_host_syscall)(
          nr == 98 ? 202 : 186, a[0], a[1], a[2], a[3], a[4], a[5]);
    case 80:  // fstat
      return GuestFstat(static_cast<int>(a[0]), reinterpret_cast<A64Stat*>(a[1]));
    case 79:  // newfstatat
      return GuestFstatat(static_cast<int>(a[0]), reinterpret_cast<const char*>(a[1]),
                          reinterpret_cast<A64Stat*>(a[2]), static_cast<int>(a[3]));
    case 21:  // epoll_ctl
      return GuestEpollCtl(static_cast<int>(a[0]), static_cast<int>(a[1]), static_cast<int>(a[2]),
                           reinterpret_cast<A64EpollEvent*>(a[3]));
    case 22:  // epoll_pwait
      return GuestEpollPwait(static_cast<int>(a[0]), reinterpret_cast<A64EpollEvent*>(a[1]),
                             static_cast<int>(a[2]), static_cast<int>(a[3]),
                             reinterpret_cast<const void*>(a[4]));
    case 168:  // getcpu(cpu, node, cache)
      if (a[0] != 0) *reinterpret_cast<uint32_t*>(a[0]) = 0;
      if (a[1] != 0) *reinterpret_cast<uint32_t*>(a[1]) = 0;
      return 0;
    default: {
      auto it = g_syscalls.find(static_cast<uint32_t>(nr));
      if (it == g_syscalls.end() || it->second.first == nullptr) break;
      int64_t r = reinterpret_cast<HostFn6>(it->second.first)(a[0], a[1], a[2], a[3], a[4], a[5]);
      if (it->second.second == 'i') return static_cast<int32_t>(r);
      if (it->second.second == 'p') return r == 0 ? -1 : static_cast<int64_t>(std::strlen(reinterpret_cast<const char*>(r)) + 1);
      return r;
    }
  }
  SetErrno(kENOSYS);
  return -1;
}

int64_t GESHER_SYSV GuestSyscall(int64_t nr, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                                 int64_t a4, int64_t a5) {
  int64_t a[6] = {a0, a1, a2, a3, a4, a5};
  return DoSyscall(static_cast<uint64_t>(nr), a);
}

// --- dlsym -----------------------------------------------------------------------
void* g_dlsym = nullptr;

void* GESHER_SYSV GuestDlsym(void* handle, const char* name) {
  if (name != nullptr) {
    if (void* g = GuestSymbol(name)) return g;
  }
  return g_dlsym != nullptr
             ? reinterpret_cast<void*(GESHER_SYSV*)(void*, const char*)>(g_dlsym)(handle, name)
             : nullptr;
}

// --- setjmp / longjmp (guest code) -------------------------------------------------
// The jmp_buf (bionic's arm64 jmp_buf is long[32]) holds x19..x30 at 0..95,
// sp at 96 and d8..d15 at 104..167. The signal mask is not saved (sigsetjmp's
// savemask is ignored).
constexpr uint32_t StpX(int rt, int rt2, int rn, int off) {
  return 0xA9000000u | ((static_cast<uint32_t>(off / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt;
}
constexpr uint32_t LdpX(int rt, int rt2, int rn, int off) { return StpX(rt, rt2, rn, off) | 0x00400000u; }
constexpr uint32_t StpD(int rt, int rt2, int rn, int off) {
  return 0x6D000000u | ((static_cast<uint32_t>(off / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt;
}
constexpr uint32_t LdpD(int rt, int rt2, int rn, int off) { return StpD(rt, rt2, rn, off) | 0x00400000u; }
constexpr uint32_t StrX(int rt, int rn, int off) { return 0xF9000000u | ((off / 8) << 10) | (rn << 5) | rt; }
constexpr uint32_t LdrX(int rt, int rn, int off) { return 0xF9400000u | ((off / 8) << 10) | (rn << 5) | rt; }

const uint32_t kSetjmp[] = {
    0x910003F0u,            // mov x16, sp
    StpX(19, 20, 0, 0),  StpX(21, 22, 0, 16), StpX(23, 24, 0, 32),
    StpX(25, 26, 0, 48), StpX(27, 28, 0, 64), StpX(29, 30, 0, 80),
    StrX(16, 0, 96),
    StpD(8, 9, 0, 104),  StpD(10, 11, 0, 120), StpD(12, 13, 0, 136), StpD(14, 15, 0, 152),
    0xD2800000u,            // mov x0, #0
    0xD65F03C0u,            // ret
};
const uint32_t kLongjmp[] = {
    LdpX(19, 20, 0, 0),  LdpX(21, 22, 0, 16), LdpX(23, 24, 0, 32),
    LdpX(25, 26, 0, 48), LdpX(27, 28, 0, 64), LdpX(29, 30, 0, 80),
    LdrX(16, 0, 96),
    0x9100021Fu,            // mov sp, x16
    LdpD(8, 9, 0, 104),  LdpD(10, 11, 0, 120), LdpD(12, 13, 0, 136), LdpD(14, 15, 0, 152),
    0xF100003Fu,            // cmp x1, #0
    0x9A9F1420u,            // csinc x0, x1, xzr, ne   (longjmp(env, 0) returns 1)
    0xD65F03C0u,            // ret
};

struct VaFn {
  const char* name;
  int index;
};
const VaFn kVaFunctions[] = {
    {"vprintf", 1},          {"vfprintf", 2},         {"vsprintf", 2},
    {"vsnprintf", 3},        {"vdprintf", 2},         {"vasprintf", 2},
    {"__vsprintf_chk", 4},   {"__vsnprintf_chk", 5},  {"vsscanf", 2},
    {"vfscanf", 2},          {"vscanf", 1},           {"vswprintf", 3},
    {"vsyslog", 2},          {"__android_log_vprint", 3},
    {"__android_log_buf_vprint", 4},                  {"__android_log_vassert", 3},
};

void Override(const char* name, void* host_wrapper) {
  if (host_wrapper != nullptr) g_overrides[name] = BindHost(host_wrapper, -1);
}

}  // namespace

void* GuestSymbol(const char* name) {
  auto it = g_overrides.find(name);
  return it == g_overrides.end() ? nullptr : it->second;
}

int64_t GuestRawSyscall(uint64_t nr, const uint64_t args[6]) {
  int64_t a[6];
  for (int i = 0; i < 6; ++i) a[i] = static_cast<int64_t>(args[i]);
  int64_t r = DoSyscall(nr, a);
  if (r == -1) {
    int* e = HostErrno();
    return -(e != nullptr && *e != 0 ? *e : kEFAULT);
  }
  return r;
}

void InitGuestLibc() {
  for (const VaFn& v : kVaFunctions) AddVaListFunction(Host(v.name), v.index);

  g_stat = Host("stat");
  g_lstat = Host("lstat");
  g_fstat = Host("fstat");
  g_fstatat = Host("fstatat");
  if (g_stat) Override("stat", reinterpret_cast<void*>(&GuestStat)), Override("stat64", reinterpret_cast<void*>(&GuestStat));
  if (g_lstat) Override("lstat", reinterpret_cast<void*>(&GuestLstat)), Override("lstat64", reinterpret_cast<void*>(&GuestLstat));
  if (g_fstat) Override("fstat", reinterpret_cast<void*>(&GuestFstat)), Override("fstat64", reinterpret_cast<void*>(&GuestFstat));
  if (g_fstatat) Override("fstatat", reinterpret_cast<void*>(&GuestFstatat)), Override("fstatat64", reinterpret_cast<void*>(&GuestFstatat));

  g_epoll_ctl = Host("epoll_ctl");
  g_epoll_wait = Host("epoll_wait");
  g_epoll_pwait = Host("epoll_pwait");
  if (g_epoll_ctl) Override("epoll_ctl", reinterpret_cast<void*>(&GuestEpollCtl));
  if (g_epoll_wait || g_epoll_pwait) {
    Override("epoll_wait", reinterpret_cast<void*>(&GuestEpollWait));
    Override("epoll_pwait", reinterpret_cast<void*>(&GuestEpollPwait));
    Override("epoll_pwait64", reinterpret_cast<void*>(&GuestEpollPwait));
  }

  g_getauxval = Host("getauxval");
  Override("getauxval", reinterpret_cast<void*>(&GuestGetauxval));

  g_host_syscall = Host("syscall");
  for (const SyscallMap& s : kSyscalls) g_syscalls[s.nr] = {Host(s.fn), s.ret};
  Override("syscall", reinterpret_cast<void*>(&GuestSyscall));

  g_dlsym = Host("dlsym");
  if (g_dlsym) Override("dlsym", reinterpret_cast<void*>(&GuestDlsym));

  void* sj = EmitGuestCode(kSetjmp, sizeof(kSetjmp) / 4);
  void* lj = EmitGuestCode(kLongjmp, sizeof(kLongjmp) / 4);
  for (const char* n : {"setjmp", "_setjmp", "sigsetjmp"}) g_overrides[n] = sj;
  for (const char* n : {"longjmp", "_longjmp", "siglongjmp"}) g_overrides[n] = lj;
}

}  // namespace gesher_arm64
