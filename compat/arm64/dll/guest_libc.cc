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
//  - mallinfo: AAPCS64 passes its struct return's address in x8;
//  - nexttoward(f): their long double is a whole q register;
//  - setjmp/longjmp: they save and restore registers, so they must be guest
//    code (AArch64, emitted here), not the host's;
//  - the floating-point environment (FPCR/FPSR, fenv_t, FE_* values);
//  - strtold-family conversions use that guest rounding/exception environment;
//  - four O_* open flags (O_DIRECTORY, O_NOFOLLOW, O_DIRECT, O_LARGEFILE);
//  - localeconv's CHAR_MAX sentinels (unsigned char on Android arm64);
//  - DNS bitstring rendering's plain-char promotion (unsigned on arm64);
//  - dlsym: returns these definitions for those names;
//  - the shim's va_list functions (vsnprintf...): registered so the guest's
//    AAPCS64 va_list is converted to System V when the guest calls them.
// The loader asks guest_symbol(name) before binding a guest import to the shim.

#include "translator.h"
#include "guest_setjmp.h"
#include "numeric_fenv.h"
#include <gesher/symbol_table.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <new>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace gesher_arm64 {

namespace {

using HostFn6 = int64_t(GESHER_SYSV*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

gesher::SymbolTable g_overrides;

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

// pthread_create inherits its caller's floating-point environment. Capture
// guest FPCR/FPSR rather than the host's restored x86 environment here.
using ThreadStart = void*(GESHER_SYSV*)(void*);
using ThreadCreate = int(GESHER_SYSV*)(void*, const void*, ThreadStart, void*);
ThreadCreate g_pthread_create = nullptr;
struct GuestThreadStart {
  ThreadStart function;
  void* argument;
  uint32_t fpcr;
  uint32_t fpsr;
  std::atomic<int32_t> references{2};
  std::atomic<int32_t> ready{0};
};
void ReleaseGuestThreadStart(GuestThreadStart* start) {
  if (start->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete start;
}
// fork()/vfork() fail with EAGAIN in guest code. The shim's fork is
// vfork-style (ADR 0055): the child runs on, and the second return goes back
// to the host frame that called fork, but the guest's registers are then
// the child's, not the parent's. A failure Linux also gives (RLIMIT_NPROC)
// is safer. Ijiami's anti-debug check forks a child that sends itself
// SIGABRT; run in this process, that killed the app (ADR 0086).
int GESHER_SYSV GuestFork() {
  SetErrno(11);  // EAGAIN
  return -1;
}

void* GESHER_SYSV StartGuestThread(void* opaque) {
  auto* start = static_cast<GuestThreadStart*>(opaque);
  ThreadStart function = start->function;
  void* argument = start->argument;
  // Creating the Env publishes its private stack before the creator can query
  // this pthread. Release the startup record before guest code, which may
  // terminate through pthread_exit rather than returning through this frame.
  SetGuestFpcr(start->fpcr);
  SetGuestFpsr(start->fpsr);
  start->ready.store(1, std::memory_order_release);
  WakeByAddressAll(&start->ready);
  ReleaseGuestThreadStart(start);
  return function(argument);
}
int GESHER_SYSV GuestPthreadCreate(void* thread, const void* attributes, ThreadStart function, void* argument) {
  auto* start = new (std::nothrow) GuestThreadStart{function, argument, GuestFpcr(), GuestFpsr()};
  if (start == nullptr) return 12;  // Android ENOMEM.
  int result = g_pthread_create(thread, attributes, StartGuestThread, start);
  if (result != 0) {
    delete start;
  } else {
    int32_t pending = 0;
    while (start->ready.load(std::memory_order_acquire) == 0)
      WaitOnAddress(&start->ready, &pending, sizeof pending, INFINITE);
    ReleaseGuestThreadStart(start);
  }
  return result;
}

// arm64's wchar_t is unsigned; x86_64's common width entry takes signed wchar32.
using WidthFn = int(GESHER_SYSV*)(uint32_t);
WidthFn g_wcwidth = nullptr;

int GESHER_SYSV GuestWcwidth(uint32_t c) {
  // All out-of-range unsigned values have the same ICU properties. Keep them
  // outside x86_64's signed C0 branch while sharing its exact Unicode logic.
  return g_wcwidth(c >= UINT32_C(0x80000000) ? UINT32_C(0x110000) : c);
}

int GESHER_SYSV GuestWcswidth(const uint32_t* s, size_t n) {
  int total = 0;
  for (size_t i = 0; i < n && s[i] != 0; ++i) {
    int width = GuestWcwidth(s[i]);
    if (width == -1) return -1;
    total += width;
  }
  return total;
}

// --- localeconv: identical LP64 pointers, architecture-specific char values ----
struct Arm64Lconv {
  const char* strings[10];
  uint8_t indicators[14];
};
static_assert(sizeof(Arm64Lconv) == 96 && offsetof(Arm64Lconv, indicators) == 80);
void* g_localeconv = nullptr;

const Arm64Lconv* GESHER_SYSV GuestLocaleconv() {
  static const Arm64Lconv locale = [] {
    using Function = const void*(GESHER_SYSV*)();
    Arm64Lconv result;
    std::memcpy(&result, reinterpret_cast<Function>(g_localeconv)(), sizeof(result));
    // Both bionic locales use CHAR_MAX for all fourteen monetary indicators.
    // The host/x86-64 char is signed (127); Android arm64 char is unsigned.
    std::memset(result.indicators, 0xff, sizeof(result.indicators));
    return result;
  }();
  return &locale;
}

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

// Traversal callbacks have the same stat-layout difference as stat().
struct GuestFtwInfo { int base, level; };
using GuestFtwFn = int(GESHER_SYSV*)(const char*, const A64Stat*, int);
using GuestNftwFn = int(GESHER_SYSV*)(const char*, const A64Stat*, int, GuestFtwInfo*);
using HostFtwCallback = int(GESHER_SYSV*)(const char*, const X64Stat*, int);
using HostNftwCallback = int(GESHER_SYSV*)(const char*, const X64Stat*, int, GuestFtwInfo*);
void* g_ftw = nullptr;
void* g_nftw = nullptr;
thread_local GuestFtwFn g_ftw_callback = nullptr;
thread_local GuestNftwFn g_nftw_callback = nullptr;
int GESHER_SYSV GuestFtwCallback(const char* path, const X64Stat* host, int type) {
  A64Stat guest;
  ToGuest(*host, &guest);
  return g_ftw_callback(path, &guest, type);
}
int GESHER_SYSV GuestNftwCallback(const char* path, const X64Stat* host, int type, GuestFtwInfo* info) {
  A64Stat guest;
  ToGuest(*host, &guest);
  return g_nftw_callback(path, &guest, type, info);
}
int GESHER_SYSV GuestFtw(const char* path, GuestFtwFn fn, int nfds) {
  if (!path || !fn) { SetErrno(22); return -1; }
  auto saved = g_ftw_callback;
  g_ftw_callback = fn;
  int result = reinterpret_cast<int(GESHER_SYSV*)(const char*, HostFtwCallback, int)>(g_ftw)(
      path, GuestFtwCallback, nfds);
  g_ftw_callback = saved;
  return result;
}
int GESHER_SYSV GuestNftw(const char* path, GuestNftwFn fn, int nfds, int flags) {
  if (!path || !fn) { SetErrno(22); return -1; }
  auto saved = g_nftw_callback;
  g_nftw_callback = fn;
  int result = reinterpret_cast<int(GESHER_SYSV*)(const char*, HostNftwCallback, int, int)>(g_nftw)(
      path, GuestNftwCallback, nfds, flags);
  g_nftw_callback = saved;
  return result;
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

// --- open flags -----------------------------------------------------------------
// Four O_* bits are numbered differently on arm64 (the generic table) and
// x86-64 (the shim's): O_DIRECTORY, O_NOFOLLOW, O_DIRECT, O_LARGEFILE.
// libc++'s std::filesystem::remove_all opens entries with O_DIRECTORY |
// O_NOFOLLOW to tell directories from files, so they must mean the same.
constexpr int kA64Directory = 040000, kA64Nofollow = 0100000, kA64Direct = 0200000,
              kA64Largefile = 0400000;
constexpr int kX64Directory = 0200000, kX64Nofollow = 0400000, kX64Direct = 040000,
              kX64Largefile = 0100000;
constexpr int kMovedBits = 040000 | 0100000 | 0200000 | 0400000;

int ToHostFlags(int f) {
  int out = f & ~kMovedBits;
  if (f & kA64Directory) out |= kX64Directory;
  if (f & kA64Nofollow) out |= kX64Nofollow;
  if (f & kA64Direct) out |= kX64Direct;
  if (f & kA64Largefile) out |= kX64Largefile;
  return out;
}
int ToGuestFlags(int f) {
  int out = f & ~kMovedBits;
  if (f & kX64Directory) out |= kA64Directory;
  if (f & kX64Nofollow) out |= kA64Nofollow;
  if (f & kX64Direct) out |= kA64Direct;
  if (f & kX64Largefile) out |= kA64Largefile;
  return out;
}

void* g_open = nullptr;
void* g_openat = nullptr;
void* g_fcntl = nullptr;
void* g_spawn_addopen = nullptr;

int GESHER_SYSV GuestOpen(const char* path, int flags, int mode) {
  return reinterpret_cast<int(GESHER_SYSV*)(const char*, int, int)>(g_open)(path, ToHostFlags(flags), mode);
}
int GESHER_SYSV GuestOpenat(int dirfd, const char* path, int flags, int mode) {
  return reinterpret_cast<int(GESHER_SYSV*)(int, const char*, int, int)>(g_openat)(
      dirfd, path, ToHostFlags(flags), mode);
}
int GESHER_SYSV GuestSpawnAddOpen(void* actions, int fd, const char* path, int flags, uint32_t mode) {
  return reinterpret_cast<int(GESHER_SYSV*)(void*, int, const char*, int, uint32_t)>(g_spawn_addopen)(
      actions, fd, path, ToHostFlags(flags), mode);
}
int64_t GESHER_SYSV GuestFcntl(int fd, int cmd, int64_t arg) {
  constexpr int kGetfl = 3, kSetfl = 4;
  if (cmd == kSetfl) arg = ToHostFlags(static_cast<int>(arg));
  int64_t r = reinterpret_cast<int64_t(GESHER_SYSV*)(int, int, int64_t)>(g_fcntl)(fd, cmd, arg);
  if (cmd == kGetfl && r >= 0) r = ToGuestFlags(static_cast<int>(r));
  return static_cast<int32_t>(r);
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
    {5, "setxattr", 'i'}, {6, "lsetxattr", 'i'}, {7, "__fsetxattr", 'i'},
    {8, "getxattr", 'l'}, {9, "lgetxattr", 'l'}, {10, "__fgetxattr", 'l'},
    {11, "listxattr", 'l'}, {12, "llistxattr", 'l'}, {13, "__flistxattr", 'l'},
    {14, "removexattr", 'i'}, {15, "lremovexattr", 'i'}, {16, "fremovexattr", 'i'},
    {17, "getcwd", 'p'},      {23, "dup", 'i'},          {24, "dup3", 'i'},
    {26, "inotify_init1", 'i'}, {27, "inotify_add_watch", 'i'}, {28, "inotify_rm_watch", 'i'},
    {32, "flock", 'i'},
    {25, "fcntl", 'i'},       {29, "ioctl", 'i'},        {34, "mkdirat", 'i'},
    {35, "unlinkat", 'i'},    {38, "renameat", 'i'},     {46, "ftruncate", 'i'},
    {48, "faccessat", 'i'},   {56, "openat", 'i'},       {57, "close", 'i'},
    {59, "pipe2", 'i'},       {62, "lseek", 'l'},        {63, "read", 'l'},
    {64, "write", 'l'},       {65, "readv", 'l'},        {66, "writev", 'l'},
    {67, "pread64", 'l'},     {68, "pwrite64", 'l'},     {78, "readlinkat", 'l'},
    {82, "fsync", 'i'},       {83, "fdatasync", 'i'},    {94, "_exit", 'i'},
    {85, "timerfd_create", 'i'}, {86, "timerfd_settime", 'i'}, {87, "timerfd_gettime", 'i'},
    {101, "nanosleep", 'i'},  {113, "clock_gettime", 'i'}, {114, "clock_getres", 'i'},
    {124, "sched_yield", 'i'}, {123, "sched_getaffinity", 'i'}, {129, "kill", 'i'},
    {131, "tgkill", 'i'},     {132, "sigaltstack", 'i'}, {160, "uname", 'i'},
    {163, "getrlimit", 'i'},
    {165, "getrusage", 'i'},  {167, "prctl", 'i'},       {169, "gettimeofday", 'i'},
    {172, "getpid", 'i'},     {173, "getppid", 'i'},     {174, "getuid", 'i'},
    {175, "geteuid", 'i'},    {176, "getgid", 'i'},      {177, "getegid", 'i'},
    {179, "sysinfo", 'i'},    {198, "socket", 'i'},      {203, "connect", 'i'},
    {215, "munmap", 'i'},     {222, "mmap", 'l'},        {226, "mprotect", 'i'},
    {228, "mlock", 'i'},      {229, "munlock", 'i'},     {230, "mlockall", 'i'},
    {231, "munlockall", 'i'},  {233, "madvise", 'i'},     {278, "getrandom", 'l'},
    {279, "memfd_create", 'i'}, {284, "mlock2", 'i'},
};
std::unordered_map<uint32_t, std::pair<void*, char>> g_syscalls;
void* g_host_syscall = nullptr;

// mprotect/mmap: the host never runs guest code (guest code pages are not
// executable, so a host call of a guest function pointer faults into its
// host_entry stub). A guest that makes pages PROT_EXEC itself - an
// anti-tamper library re-protecting its own text, libtaz_full.so - would make
// them host-executable: a host call of a guest pointer there (pthread_once,
// qsort, a JNI callback) then runs AArch64 bytes as x86 and dies with SIGILL
// (or worse) instead. PROT_EXEC becomes PROT_READ: the translator reads it,
// and the pages become guest code. A guest may write code into memory that is
// no guest library's: Facebook's force_dlopen (libforce_dlopen.so, WhatsApp)
// writes a `blr x2` trampoline over the string gai_strerror(EAI_NONAME)
// returns, so that dlopen sees a caller inside libc, then jumps to it.
//
// The translator's own pages hold the trampolines of host functions, and it
// adds one each time the guest binds another. A guest that hooks a libc
// function in place (WhatsApp's libessential) re-protects that trampoline's
// page PROT_READ|PROT_EXEC when done. Those pages stay read-write, or the
// next binding (the next library's imports) faults in the host.
constexpr int kProtRead = 1;
constexpr int kProtWrite = 2;
constexpr int kProtExec = 4;
void* g_mprotect = nullptr;
void* g_mmap = nullptr;

int GuestProt(int prot) { return (prot & kProtExec) != 0 ? (prot & ~kProtExec) | kProtRead : prot; }

int GESHER_SYSV GuestMprotect(void* addr, size_t len, int prot) {
  int host_prot = GuestProt(prot);
  if (InTranslatorCode(addr, len)) host_prot = kProtRead | kProtWrite;
  int r = reinterpret_cast<int(GESHER_SYSV*)(void*, size_t, int)>(g_mprotect)(addr, len,
                                                                              host_prot);
  if (r == 0 && (prot & kProtExec) != 0) AddGuestCode(addr, len);
  return r;
}

void* GESHER_SYSV GuestMmap(void* addr, size_t len, int prot, int flags, int fd, int64_t off) {
  void* p = reinterpret_cast<void*(GESHER_SYSV*)(void*, size_t, int, int, int, int64_t)>(g_mmap)(
      addr, len, GuestProt(prot), flags, fd, off);
  if (p != reinterpret_cast<void*>(-1) && (prot & kProtExec) != 0) AddGuestCode(p, len);
  return p;
}

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
    case 56:  // openat
      if (g_openat == nullptr) break;
      return GuestOpenat(static_cast<int>(a[0]), reinterpret_cast<const char*>(a[1]),
                         static_cast<int>(a[2]), static_cast<int>(a[3]));
    case 25:  // fcntl
      if (g_fcntl == nullptr) break;
      return GuestFcntl(static_cast<int>(a[0]), static_cast<int>(a[1]), a[2]);
    case 226:  // mprotect
      if (g_mprotect == nullptr) break;
      return GuestMprotect(reinterpret_cast<void*>(a[0]), static_cast<size_t>(a[1]),
                           static_cast<int>(a[2]));
    case 222:  // mmap
      if (g_mmap == nullptr) break;
      return reinterpret_cast<int64_t>(GuestMmap(reinterpret_cast<void*>(a[0]),
                                                 static_cast<size_t>(a[1]), static_cast<int>(a[2]),
                                                 static_cast<int>(a[3]), static_cast<int>(a[4]),
                                                 a[5]));
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

// --- dlsym --------------------------------------------------------------------------
// The shim's dlopen/dlsym serve the guest too: the platform's libraries
// (libEGL.so, libOpenSLES.so, ...) by name or /system/lib64 path, and its own
// symbols behind a library's handle or RTLD_DEFAULT (compat/bionic,
// ADR 0057). Only the names this file defines differently for the guest are
// answered here first. The host function pointers the shim returns are called
// by the guest directly (the translator's host-pointer path).
void* g_dlsym = nullptr;

void* GESHER_SYSV GuestDlsym(void* handle, const char* name) {
  if (name == nullptr) return nullptr;
  if (void* g = GuestSymbol(name)) return g;
  return g_dlsym != nullptr
             ? reinterpret_cast<void*(GESHER_SYSV*)(void*, const char*)>(g_dlsym)(handle, name)
             : nullptr;
}

// --- setjmp / longjmp (guest code) -------------------------------------------------
// Use the same Bionic provider as sigprocmask and signal delivery. A private
// translator mask would diverge from ordinary guest calls.
using JumpMaskFunction = int(GESHER_SYSV*)(int, const uint64_t*, uint64_t*);
JumpMaskFunction g_jump_sigprocmask = nullptr;
using JumpAbortFunction = void(GESHER_SYSV*)();
JumpAbortFunction g_jump_abort = nullptr;

[[noreturn]] void JumpAbort() {
  // Preserve the shared Bionic abort path, including a guest SIGABRT handler.
  if (g_jump_abort != nullptr) g_jump_abort();
  std::abort();  // Incomplete standalone host, or a broken returning provider.
}

// Bionic's __bionic_setjmp_cookie_get takes the flag as a signed long and
// reports it with %ld, so sigsetjmp(env, -1) reports -1.
[[noreturn]] void GESHER_SYSV JumpInvalidFlag(int64_t flag) {
  std::fprintf(stderr, "gesher arm64: unexpected sigflag value: %lld\n",
               static_cast<long long>(flag));
  JumpAbort();
}

// Bionic's __bionic_setjmp_checksum_mismatch: a corrupted jmp_buf aborts
// before longjmp changes the mask or any register.
[[noreturn]] void GESHER_SYSV JumpChecksumMismatch() {
  std::fputs("gesher arm64: setjmp checksum mismatch\n", stderr);
  JumpAbort();
}

int GESHER_SYSV JumpSigprocmask(int how, const uint64_t* set, uint64_t* old) {
  // The normal runtime always supplies this core libc entry. Standalone DLL
  // fixtures may omit it; a mask-saving jump must not report success in an
  // incomplete host. _setjmp/sigsetjmp(..., 0) do not need the provider.
  if (g_jump_sigprocmask == nullptr) {
    std::fputs("gesher arm64: setjmp signal-mask provider unavailable\n", stderr);
    JumpAbort();
  }
  int rc = g_jump_sigprocmask(how, set, old);
  if (rc != 0) {
    std::fprintf(stderr, "gesher arm64: setjmp signal-mask provider failed: sigprocmask(%d) returned %d\n",
                 how, rc);
    JumpAbort();
  }
  return 0;
}

// --- the floating-point environment ------------------------------------------------
// Rounding uses the actual per-thread FPCR, including direct guest MSR.
// Sticky exception flags and environment snapshots use the actual FPSR.
// The pinned ARM LP64 environment is two 32-bit registers, unlike x86 fenv_t.
int GESHER_SYSV FeSetround(int mode) {
  if (mode < 0 || mode > 3) return -1;
  uint32_t fpcr = GuestFpcr();
  uint32_t next = (fpcr & ~(UINT32_C(3) << 22)) | (static_cast<uint32_t>(mode) << 22);
  if (next != fpcr) SetGuestFpcr(next);
  return 0;
}
int GESHER_SYSV FeGetround() { return (GuestFpcr() >> 22) & 3; }
int GESHER_SYSV FeZero() { return 0; }
constexpr uint32_t kArmFeAll = 0x9f;  // Includes Android ARM FE_DENORMAL (0x80).
int GESHER_SYSV FeTestexcept(int excepts) {
  return GuestFpsr() & (static_cast<uint32_t>(excepts) & kArmFeAll);
}
int GESHER_SYSV FeClearexcept(int excepts) {
  SetGuestFpsr(GuestFpsr() & ~(static_cast<uint32_t>(excepts) & kArmFeAll));
  return 0;
}
struct ArmFenv { uint32_t control; uint32_t status; };
static_assert(sizeof(ArmFenv) == 8);
const ArmFenv kArmDefaultEnv = {0, 0};
int GESHER_SYSV FeGetenv(ArmFenv* env) {
  env->control = GuestFpcr();
  env->status = GuestFpsr();
  return 0;
}
int GESHER_SYSV FeSetenv(const ArmFenv* env) {
  if (env->control != GuestFpcr()) SetGuestFpcr(env->control);
  SetGuestFpsr(env->status);
  return 0;
}
int GESHER_SYSV FeHoldexcept(ArmFenv* env) {
  FeGetenv(env);
  SetGuestFpsr(GuestFpsr() & ~kArmFeAll);
  return 0;
}
int GESHER_SYSV FeUpdateenv(const ArmFenv* env) {
  uint32_t excepts = GuestFpsr() & kArmFeAll;
  FeSetenv(env);
  SetGuestFpsr(GuestFpsr() | excepts);
  return 0;
}
int GESHER_SYSV FeGetexceptflag(uint32_t* flag, int excepts) {
  *flag = GuestFpsr() & (static_cast<uint32_t>(excepts) & kArmFeAll);
  return 0;
}
int GESHER_SYSV FeSetexceptflag(const uint32_t* flag, int excepts) {
  uint32_t mask = static_cast<uint32_t>(excepts) & kArmFeAll;
  SetGuestFpsr((GuestFpsr() & ~mask) | (*flag & mask));
  return 0;
}
int GESHER_SYSV FeRaiseexcept(int excepts) {
  uint32_t flag = static_cast<uint32_t>(excepts);
  return FeSetexceptflag(&flag, excepts);
}

// The shim represents Android LP64 long double as a 128-bit vector returned
// in XMM0; the Windows C++ long double type has a different representation.
typedef int32_t QuadResult __attribute__((vector_size(16)));
using StrtoldFn = QuadResult(GESHER_SYSV*)(const char*, char**);
using StrtoldLFn = QuadResult(GESHER_SYSV*)(const char*, char**, void*);
using WcstoldFn = QuadResult(GESHER_SYSV*)(const uint32_t*, uint32_t**);
using WcstoldLFn = QuadResult(GESHER_SYSV*)(const uint32_t*, uint32_t**, void*);
StrtoldFn g_strtold = nullptr;
StrtoldLFn g_strtold_l = nullptr;
WcstoldFn g_wcstold = nullptr;
WcstoldLFn g_wcstold_l = nullptr;

template <typename Fn, typename... Args>
QuadResult ParseWithGuestFenv(Fn fn, Args... args) {
  NumericFenvScope numeric_fenv{GuestFpcr()};
  const QuadResult result = fn(args...);
  // Keep every prior guest status bit, including IDC and QC. Copy exceptions
  // actually raised by the conversion, not flags inferred from its result:
  // Bionic's gdtoa does not raise IEEE flags for every rounded value.
  SetGuestFpsr(GuestFpsr() | numeric_fenv.RaisedArmExceptions());
  return result;
}

QuadResult GESHER_SYSV GuestStrtold(const char* text, char** end) {
  return ParseWithGuestFenv(g_strtold, text, end);
}
QuadResult GESHER_SYSV GuestStrtoldL(const char* text, char** end, void* locale) {
  return ParseWithGuestFenv(g_strtold_l, text, end, locale);
}
QuadResult GESHER_SYSV GuestWcstold(const uint32_t* text, uint32_t** end) {
  return ParseWithGuestFenv(g_wcstold, text, end);
}
QuadResult GESHER_SYSV GuestWcstoldL(const uint32_t* text, uint32_t** end, void* locale) {
  return ParseWithGuestFenv(g_wcstold_l, text, end, locale);
}

struct Fallback {
  const char* name;
  void* fn;
};
#define FB(n, f) {n, reinterpret_cast<void*>(&f)}
const Fallback kFenv[] = {
    FB("fesetround", FeSetround),   FB("fegetround", FeGetround),
    FB("fetestexcept", FeTestexcept), FB("feclearexcept", FeClearexcept),
    FB("feraiseexcept", FeRaiseexcept), FB("fegetenv", FeGetenv),
    FB("fesetenv", FeSetenv),       FB("feholdexcept", FeHoldexcept),
    FB("feupdateenv", FeUpdateenv), FB("fegetexceptflag", FeGetexceptflag),
    FB("fesetexceptflag", FeSetexceptflag), FB("fegetexcept", FeZero),
};
#undef FB

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
    {"vwprintf", 1},         {"vfwprintf", 2},        {"gzvprintf", 2},
};

void Override(const char* name, void* host_wrapper) {
  if (host_wrapper != nullptr) g_overrides[name] = BindHost(host_wrapper, -1);
}

// A mismatched host must not silently route an ARM64 call to the x86 model.
int64_t GESHER_SYSV MissingArm64DnsBridge(int64_t, int64_t, int64_t,
                                           int64_t, int64_t, int64_t) {
  SetErrno(38);  // Android ENOSYS
  return -1;
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

bool InitGuestLibc() {
  for (const VaFn& v : kVaFunctions) AddVaListFunction(Host(v.name), v.index);

  AddPointerVariadicFunction(Host("sscanf"), Host("vsscanf"), 2);
  AddPointerVariadicFunction(Host("fscanf"), Host("vfscanf"), 2);
  AddPointerVariadicFunction(Host("scanf"), Host("vscanf"), 1);
  AddPointerVariadicFunction(Host("execl"), Host("__gesher_vexecl"), 2);

  for (const char* name : {"ns_name_ntop", "ns_name_uncompress", "dn_expand"}) {
    std::string entry = std::string("__gesher_arm64_") + name;
    void* bridge = Host(entry.c_str());
    Override(name, bridge ? bridge : reinterpret_cast<void*>(&MissingArm64DnsBridge));
  }

  g_pthread_create = reinterpret_cast<ThreadCreate>(Host("pthread_create"));
  if (g_pthread_create) Override("pthread_create", reinterpret_cast<void*>(&GuestPthreadCreate));
  if (void* getattr = Host("__gesher_arm64_pthread_getattr_np"))
    Override("pthread_getattr_np", getattr);

  g_localeconv = Host("localeconv");
  if (g_localeconv) Override("localeconv", reinterpret_cast<void*>(&GuestLocaleconv));

  g_stat = Host("stat");
  g_lstat = Host("lstat");
  g_fstat = Host("fstat");
  g_fstatat = Host("fstatat");
  if (g_stat) Override("stat", reinterpret_cast<void*>(&GuestStat)), Override("stat64", reinterpret_cast<void*>(&GuestStat));
  if (g_lstat) Override("lstat", reinterpret_cast<void*>(&GuestLstat)), Override("lstat64", reinterpret_cast<void*>(&GuestLstat));
  if (g_fstat) Override("fstat", reinterpret_cast<void*>(&GuestFstat)), Override("fstat64", reinterpret_cast<void*>(&GuestFstat));
  if (g_fstatat) Override("fstatat", reinterpret_cast<void*>(&GuestFstatat)), Override("fstatat64", reinterpret_cast<void*>(&GuestFstatat));

  g_ftw = Host("ftw");
  g_nftw = Host("nftw");
  if (g_ftw) for (const char* name : {"ftw", "ftw64"}) Override(name, reinterpret_cast<void*>(&GuestFtw));
  if (g_nftw) for (const char* name : {"nftw", "nftw64"}) Override(name, reinterpret_cast<void*>(&GuestNftw));

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
  g_wcwidth = reinterpret_cast<WidthFn>(Host("wcwidth"));
  if (g_wcwidth != nullptr) {
    Override("wcwidth", reinterpret_cast<void*>(&GuestWcwidth));
    Override("wcswidth", reinterpret_cast<void*>(&GuestWcswidth));
  }
  Override("getauxval", reinterpret_cast<void*>(&GuestGetauxval));

  g_mprotect = Host("mprotect");
  g_mmap = Host("mmap");
  if (g_mprotect) Override("mprotect", reinterpret_cast<void*>(&GuestMprotect));
  if (g_mmap) {
    Override("mmap", reinterpret_cast<void*>(&GuestMmap));
    Override("mmap64", reinterpret_cast<void*>(&GuestMmap));
  }

  g_host_syscall = Host("syscall");
  for (const SyscallMap& s : kSyscalls) g_syscalls[s.nr] = {Host(s.fn), s.ret};
  Override("syscall", reinterpret_cast<void*>(&GuestSyscall));

  g_open = Host("open");
  g_openat = Host("openat");
  g_fcntl = Host("fcntl");
  for (const char* n : {"fork", "vfork"}) Override(n, reinterpret_cast<void*>(&GuestFork));

  g_spawn_addopen = Host("posix_spawn_file_actions_addopen");
  if (g_open) {
    for (const char* n : {"open", "open64", "__open_2"}) Override(n, reinterpret_cast<void*>(&GuestOpen));
  }
  if (g_openat) {
    for (const char* n : {"openat", "openat64", "__openat_2"}) Override(n, reinterpret_cast<void*>(&GuestOpenat));
  }
  if (g_fcntl) Override("fcntl", reinterpret_cast<void*>(&GuestFcntl));
  if (g_spawn_addopen) Override("posix_spawn_file_actions_addopen", reinterpret_cast<void*>(&GuestSpawnAddOpen));

  g_dlsym = Host("dlsym");
  if (g_dlsym) Override("dlsym", reinterpret_cast<void*>(&GuestDlsym));

  for (const Fallback& f : kFenv) Override(f.name, f.fn);
  g_strtold = reinterpret_cast<StrtoldFn>(Host("strtold"));
  g_strtold_l = reinterpret_cast<StrtoldLFn>(Host("strtold_l"));
  g_wcstold = reinterpret_cast<WcstoldFn>(Host("wcstold"));
  g_wcstold_l = reinterpret_cast<WcstoldLFn>(Host("wcstold_l"));
  if (g_strtold) Override("strtold", reinterpret_cast<void*>(&GuestStrtold));
  if (g_strtold_l) Override("strtold_l", reinterpret_cast<void*>(&GuestStrtoldL));
  if (g_wcstold) Override("wcstold", reinterpret_cast<void*>(&GuestWcstold));
  if (g_wcstold_l) Override("wcstold_l", reinterpret_cast<void*>(&GuestWcstoldL));
  // Data imports and dlsym must return the ARM object itself, never a callable
  // trampoline or the common host shim's different x86 environment layout.
  g_overrides["__fe_dfl_env"] = const_cast<ArmFenv*>(&kArmDefaultEnv);

  g_jump_sigprocmask = reinterpret_cast<JumpMaskFunction>(Host("sigprocmask"));
  g_jump_abort = reinterpret_cast<JumpAbortFunction>(Host("abort"));
  // The emitted jump code branches to these trampolines unconditionally, and
  // a missing jump override would fall back to the x64 host's incompatible
  // jmp_buf. Refuse to initialize rather than emit a branch to address 0.
  void* mask_entry = BindHost(reinterpret_cast<void*>(&JumpSigprocmask), -1);
  void* invalid_entry = BindHost(reinterpret_cast<void*>(&JumpInvalidFlag), -1);
  void* mismatch_entry = BindHost(reinterpret_cast<void*>(&JumpChecksumMismatch), -1);
  if (mask_entry == nullptr || invalid_entry == nullptr || mismatch_entry == nullptr) {
    std::fputs("gesher arm64: cannot bind the setjmp/longjmp host entries\n", stderr);
    return false;
  }
  uint64_t mask = reinterpret_cast<uint64_t>(mask_entry);
  uint64_t invalid_flag = reinterpret_cast<uint64_t>(invalid_entry);
  struct JumpEntry { const char* name; std::vector<uint32_t> code; };
  const JumpEntry jumps[] = {
    {"setjmp", jump::Save(mask, jump::SaveMask::Always, invalid_flag)},
    {"_setjmp", jump::Save(mask, jump::SaveMask::Never, invalid_flag)},
    {"sigsetjmp", jump::Save(mask, jump::SaveMask::Argument, invalid_flag)},
    {"longjmp", jump::Restore(mask, reinterpret_cast<uint64_t>(mismatch_entry))},
  };
  void* emitted[4] = {};
  for (size_t i = 0; i < 4; ++i) {
    emitted[i] = EmitGuestCode(jumps[i].code.data(), jumps[i].code.size());
    if (emitted[i] == nullptr) {
      std::fprintf(stderr, "gesher arm64: cannot emit guest code for %s\n", jumps[i].name);
      return false;
    }
  }
  for (size_t i = 0; i < 3; ++i) g_overrides[jumps[i].name] = emitted[i];
  for (const char* name : {"longjmp", "_longjmp", "siglongjmp"}) g_overrides[name] = emitted[3];

  // mallinfo returns an 80-byte struct in memory: AAPCS64 passes its address
  // in x8, System V as the hidden first argument (rdi), which the bridge fills
  // from x0. A guest stub moves x8 to x0 and goes on to the shim's mallinfo.
  if (void* mallinfo = Host("mallinfo")) {
    void* entry = BindHost(mallinfo, -1);
    if (entry == nullptr) {
      std::fputs("gesher arm64: cannot bind the mallinfo host entry\n", stderr);
      return false;
    }
    const uint64_t host = reinterpret_cast<uint64_t>(entry);
    const uint32_t stub[] = {
        0xAA0803E0u,  // mov x0, x8
        0x58000070u,  // ldr x16, the address below
        0xD61F0200u,  // br x16
        0xD503201Fu,  // nop (aligns the address)
        static_cast<uint32_t>(host), static_cast<uint32_t>(host >> 32),
    };
    void* code = EmitGuestCode(stub, sizeof(stub) / 4);
    if (code == nullptr) {
      std::fputs("gesher arm64: cannot emit guest code for mallinfo\n", stderr);
      return false;
    }
    g_overrides["mallinfo"] = code;
  }

  // nexttoward(f)'s long double is a binary128 in q1, of which the bridge
  // carries only the low 64 bits: a guest stub stores q1 and passes its
  // address (x0) to the shim's pointer form; x stays in v0.
  for (const char* name : {"nexttoward", "nexttowardf"}) {
    void* fn = Host((std::string("__gesher_arm64_") + name).c_str());
    if (fn == nullptr) continue;
    void* entry = BindHost(fn, -1);
    if (entry == nullptr) {
      std::fprintf(stderr, "gesher arm64: cannot bind the %s host entry\n", name);
      return false;
    }
    const uint64_t host = reinterpret_cast<uint64_t>(entry);
    const uint32_t stub[] = {
        0xA9BE7BFDu,  // stp x29, x30, [sp, #-32]!
        0x3D8007E1u,  // str q1, [sp, #16]
        0x910043E0u,  // add x0, sp, #16
        0x580000B0u,  // ldr x16, the address below
        0xD63F0200u,  // blr x16
        0xA8C27BFDu,  // ldp x29, x30, [sp], #32
        0xD65F03C0u,  // ret
        0xD503201Fu,  // nop (aligns the address)
        static_cast<uint32_t>(host), static_cast<uint32_t>(host >> 32),
    };
    void* code = EmitGuestCode(stub, sizeof(stub) / 4);
    if (code == nullptr) {
      std::fprintf(stderr, "gesher arm64: cannot emit guest code for %s\n", name);
      return false;
    }
    g_overrides[name] = code;
  }
  return true;
}

}  // namespace gesher_arm64
