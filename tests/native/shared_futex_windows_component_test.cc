// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Standalone fixture for the production mmap and futex path. Only the
// descriptor capture, binder, memfd seal, signal delivery, and fork
// registration are boundaries. The Win32 mapping and synchronization
// operations run unchanged, with separate processes and separate file opens.
#include <gesher/clocks.h>
#include <gesher/file_description.h>
#include <gesher/fork.h>
#include <gesher/futex_interrupt.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include <tuple>
#include "revocable_internal.h"
#include "shim_internal.h"

namespace gesher {
DescriptorSnapshot::~DescriptorSnapshot() { CloseHandle(handle); }
DescriptorSnapshotPtr CaptureDescriptor(int fd) {
  auto snapshot = std::make_shared<DescriptorSnapshot>();
  if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(_get_osfhandle(fd)),
                       GetCurrentProcess(), &snapshot->handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
    return {};
  return snapshot;
}
bool IsBinderFd(int) { return false; }
bool BinderFdOwnedHere(int) { return false; }
void SetBinderMapping(int, void*, size_t) {}
int FileDescriptionFlags(const DescriptorSnapshotPtr&) { return O_RDWR; }
bool FileDescriptionIsRevocable(const DescriptorSnapshotPtr&) { return false; }
FileOperationLease::FileOperationLease(const DescriptorSnapshotPtr&) {}
FileOperationLease::~FileOperationLease() = default;
int MemfdSharedMapHandleError(HANDLE, bool, bool*, std::shared_ptr<void>*) { return 0; }
void RenewMemfdWriteLeaseInChild(void*) {}
void RegisterForkLock(int, const char*, ForkLockGet, const ForkLockOps*) {}
bool InheritedAtFork(HANDLE) { return false; }
void ThreadLeaving() { abort(); }
bool ForkSetHeld() { abort(); }
}
namespace gesher::elf {
bool SetupThreadTls() { abort(); }
}
namespace gesher::bionic {
gesher_futex_interrupt* AttachSignalInterrupt() { return nullptr; }
void DetachSignalInterrupt() { abort(); }
int DeliverAfterInterrupt() { abort(); }
uint32_t MillisUntil(const LinuxTimespec*, bool) { abort(); }
bool InterruptibleAddressWait(volatile void*, const void*, size_t, unsigned long) { abort(); }
}
extern "C" int gesher_pthread_key_create(pthread_key_t*, void (*)(void*)) { abort(); }
extern "C" int memfd_create(const char*, unsigned) { errno = ENOSYS; return -1; }
extern "C" int getrlimit(int, struct rlimit*) { errno = ENOSYS; return -1; }

namespace {
constexpr size_t kPage = 4096;
constexpr size_t kBytes = (64 * 1024) + kPage;
using namespace gesher::bionic;
struct Shared {
  int ready;
  int word;
  int other;
  int stage;
  int condition_met;
  bionic_pthread_mutex_t mutex;
  bionic_pthread_cond_t condition;
  bionic_pthread_rwlock_t rwlock;
};

void Require(bool ok, const char* description, long actual = 0) {
  if (ok) return;
  fprintf(stderr, "%lu: %s, actual=%ld errno=%d Windows=%lu\n",
          GetCurrentProcessId(), description, actual, errno, GetLastError());
  exit(1);
}

Shared* View(const wchar_t* path, int prot, off_t offset, uintptr_t preferred, size_t length) {
  HANDLE h = CreateFileW(path, GENERIC_READ | (prot & PROT_WRITE ? GENERIC_WRITE : 0),
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(h != INVALID_HANDLE_VALUE, "open backing file");
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), prot & PROT_WRITE ? _O_RDWR : _O_RDONLY);
  Require(fd >= 0, "allocate fixture descriptor");
  void* view = mmap(reinterpret_cast<void*>(preferred), length, prot,
                    MAP_SHARED | MAP_FIXED_NOREPLACE, fd, offset);
  Require(view != MAP_FAILED, "map backing file");
  Require(_close(fd) == 0, "close mapped descriptor");
  Require(reinterpret_cast<uintptr_t>(view) == preferred, "fixture map address");
  return static_cast<Shared*>(view);
}

long Futex(int* word, int operation, int expected, const timespec* time = nullptr, int mask = 0) {
  return syscall(SYS_futex, word, operation, expected, time, nullptr, mask);
}

template <typename... Args>
int Pthread(const char* name, Args... args) {
  static const SymbolTable table = [] {
    SymbolTable symbols;
    RegisterPthread(symbols);
    return symbols;
  }();
  auto it = table.find(name);
  Require(it != table.end(), name);
  using Fn = int (BIONIC_SYSV*)(Args...);
  return reinterpret_cast<Fn>(it->second)(args...);
}

LinuxTimespec MonotonicDeadline() {
  timespec now;
  Require(gesher::AndroidClockGettime(CLOCK_MONOTONIC, &now) == 0, "read Android monotonic clock");
  return {now.tv_sec + 5, now.tv_nsec};
}

void PthreadChild(Shared* view) {
  InterlockedExchange(reinterpret_cast<LONG*>(&view->stage), 1);
  const auto deadline = MonotonicDeadline();
  Require(Pthread("pthread_mutex_clocklock", &view->mutex, 1, &deadline) == 0,
          "wait for parent to release process-shared mutex");
  InterlockedExchange(reinterpret_cast<LONG*>(&view->stage), 2);
  while (!view->condition_met) {
    Require(Pthread("pthread_cond_timedwait", &view->condition, &view->mutex, &deadline) == 0,
            "wait for parent to signal process-shared condition");
  }
  Require(Pthread("pthread_mutex_unlock", &view->mutex) == 0, "release condition mutex");
  InterlockedExchange(reinterpret_cast<LONG*>(&view->stage), 3);
  Require(Pthread("pthread_rwlock_clockrdlock", &view->rwlock, 1, &deadline) == 0,
          "wait for parent to release process-shared writer lock");
  Require(Pthread("pthread_rwlock_unlock", &view->rwlock) == 0, "release shared reader lock");
  puts("N29 Bionic pshared mutex, condition, and rwlock waiter PASS");
}

void Child(const wchar_t* path, int mask, int mode) {
  auto* base = View(path, PROT_READ | PROT_WRITE, 0, 0x20000000000, kBytes);
  auto* view = reinterpret_cast<Shared*>(reinterpret_cast<char*>(base) + kBytes - kPage);
  if (mode == 2) {
    PthreadChild(view);
    Require(munmap(base, kBytes) == 0, "unmap shared pthread objects");
    return;
  }
  const bool basic = mode == 1;
  InterlockedIncrement(reinterpret_cast<LONG*>(&view->ready));
  timespec relative{5, 0};
  timespec absolute{};
  Require(clock_gettime(CLOCK_REALTIME, &absolute) == 0, "get realtime");
  absolute.tv_sec += 5;
  long result = Futex(&view->word, basic ? FUTEX_WAIT : FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME,
                      0, basic ? &relative : &absolute, mask);
  Require(result == 0, "cross-process waiter receives its wake", result);
  Require(munmap(base, kBytes) == 0, "unmap");
  printf("child mask=%d map=%p PASS\n", mask, static_cast<void*>(view));
}

HANDLE Spawn(const wchar_t* path, int mask, int mode) {
  wchar_t exe[MAX_PATH];
  wchar_t cmd[2 * MAX_PATH + 48];
  Require(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0, "query fixture path");
  swprintf(cmd, sizeof(cmd) / sizeof(*cmd), L"\"%ls\" \"%ls\" %d %d", exe, path, mask, mode);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  Require(CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info),
          "spawn separate Windows process");
  CloseHandle(info.hThread);
  return info.hProcess;
}

void AwaitReady(Shared* view, LONG target) {
  const ULONGLONG until = GetTickCount64() + 3500;
  while (InterlockedCompareExchange(reinterpret_cast<LONG*>(&view->ready), 0, 0) < target) {
    Require(GetTickCount64() < until, "wait for child setup");
    Sleep(1);
  }
}

void AwaitWake(Shared* view, int mask, bool basic = false) {
  const ULONGLONG until = GetTickCount64() + 3500;
  for (;;) {
    const long result = Futex(&view->word, basic ? FUTEX_WAKE : FUTEX_WAKE_BITSET, 1, nullptr, mask);
    Require(result == 0 || result == 1, "wake selects at most one waiter", result);
    if (result == 1) return;
    Require(GetTickCount64() < until, "wait for matching queue enrollment");
    Sleep(1);
  }
}

void Reap(HANDLE process, DWORD expected) {
  DWORD code = 100;
  Require(WaitForSingleObject(process, 7000) == WAIT_OBJECT_0, "child exits");
  Require(GetExitCodeProcess(process, &code) && code == expected, "child exit status", code);
  CloseHandle(process);
}
void AwaitStage(Shared* view, LONG target) {
  const ULONGLONG until = GetTickCount64() + 3500;
  while (InterlockedCompareExchange(reinterpret_cast<LONG*>(&view->stage), 0, 0) < target) {
    Require(GetTickCount64() < until, "wait for pthread contention stage", target);
    Sleep(1);
  }
}

void TestPshared(Shared* view, const wchar_t* child_path) {
  bionic_pthread_mutexattr_t mutex_attr;
  bionic_pthread_condattr_t cond_attr;
  bionic_pthread_rwlockattr_t rw_attr;
  Require(Pthread("pthread_mutexattr_init", &mutex_attr) == 0, "init mutex attr");
  Require(Pthread("pthread_condattr_init", &cond_attr) == 0, "init condition attr");
  Require(Pthread("pthread_rwlockattr_init", &rw_attr) == 0, "init rwlock attr");
  int value = -1;
  for (auto [set, get, attr] : {
           std::tuple{"pthread_mutexattr_setpshared", "pthread_mutexattr_getpshared", &mutex_attr},
           std::tuple{"pthread_condattr_setpshared", "pthread_condattr_getpshared", &cond_attr},
           std::tuple{"pthread_rwlockattr_setpshared", "pthread_rwlockattr_getpshared", &rw_attr}}) {
    const int64_t* const_attr = attr;
    Require(Pthread(get, const_attr, &value) == 0 && value == 0, "private attribute default");
    Require(Pthread(set, attr, 8) == EINVAL, "invalid shared attribute");
    Require(Pthread(set, attr, 1) == 0, "set process-shared attribute");
    Require(Pthread(get, const_attr, &value) == 0 && value == 1, "get process-shared attribute");
  }
  Require(Pthread("pthread_condattr_setclock", &cond_attr, 1) == 0 &&
          Pthread("pthread_condattr_getclock", static_cast<const int64_t*>(&cond_attr), &value) == 0 &&
          value == 1, "condition clock and pshared bits coexist");
  Require(Pthread("pthread_mutex_init", &view->mutex,
                  static_cast<const int64_t*>(&mutex_attr)) == 0, "init shared mutex");
  Require(Pthread("pthread_cond_init", &view->condition,
                  static_cast<const int64_t*>(&cond_attr)) == 0, "init shared condition");
  Require(Pthread("pthread_rwlock_init", &view->rwlock,
                  static_cast<const int64_t*>(&rw_attr)) == 0, "init shared rwlock");
  Require(Pthread("pthread_mutex_lock", &view->mutex) == 0, "take mutex before child");
  Require(Pthread("pthread_rwlock_wrlock", &view->rwlock) == 0, "take writer lock before child");
  HANDLE child = Spawn(child_path, 0, 2);
  AwaitStage(view, 1);
  Sleep(100);
  Require(InterlockedCompareExchange(reinterpret_cast<LONG*>(&view->stage), 0, 0) == 1,
          "child waited on the mutex");
  Require(Pthread("pthread_mutex_unlock", &view->mutex) == 0, "wake mutex waiter");
  AwaitStage(view, 2);
  const auto deadline = MonotonicDeadline();
  Require(Pthread("pthread_mutex_clocklock", &view->mutex, 1, &deadline) == 0,
          "condition wait released the mutex");
  view->condition_met = 1;
  Require(Pthread("pthread_cond_signal", &view->condition) == 0, "wake condition waiter");
  Require(Pthread("pthread_mutex_unlock", &view->mutex) == 0, "release condition mutex");
  AwaitStage(view, 3);
  Sleep(100);
  Require(WaitForSingleObject(child, 0) == WAIT_TIMEOUT, "child waited on the writer lock");
  Require(Pthread("pthread_rwlock_unlock", &view->rwlock) == 0, "wake reader");
  Reap(child, 0);
  puts("N29 Bionic pshared synchronization across processes PASS");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 4) { Child(argv[1], _wtoi(argv[2]), _wtoi(argv[3])); return 0; }
  Require(argc == 1, "fixture arguments");
  wchar_t dir[MAX_PATH], path[MAX_PATH], hardlink[MAX_PATH];
  Require(GetTempPathW(MAX_PATH, dir) != 0 && GetTempFileNameW(dir, L"gft", 0, path),
          "create backing file path");
  swprintf(hardlink, MAX_PATH, L"%ls.link", path);
  HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(file != INVALID_HANDLE_VALUE, "create backing file");
  LARGE_INTEGER size{};
  size.QuadPart = kBytes;
  Require(SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file), "size backing file");
  CloseHandle(file);
  Require(CreateHardLinkW(hardlink, path, nullptr), "create hard link");

  auto* view = View(path, PROT_READ | PROT_WRITE, kBytes - kPage, 0x30000000000, kPage);
  timespec zero{};
  Require(Futex(&view->word, FUTEX_WAIT, 1, &zero) == -1 && errno == EAGAIN, "mismatch");
  Require(Futex(&view->word, FUTEX_WAIT, 0, &zero) == -1 && errno == ETIMEDOUT, "timeout");
  Require(Futex(&view->word, FUTEX_WAKE, 1) == 0, "timeout removed the slot");

  HANDLE first = Spawn(hardlink, 1, false), second = Spawn(hardlink, 2, false);
  AwaitReady(view, 2);
  Require(Futex(&view->other, FUTEX_WAKE_BITSET, 1, nullptr, 1) == 0, "offset isolation");
  Require(Futex(&view->word, FUTEX_WAKE_BITSET, 1, nullptr, 4) == 0, "mask isolation");
  Require(Futex(&view->word, FUTEX_WAKE_PRIVATE, 1) == 0, "private isolation");
  auto* read_only = View(path, PROT_READ, kBytes - kPage, 0x40000000000, kPage);
  AwaitWake(read_only, 2);
  Reap(second, 0);
  Require(WaitForSingleObject(first, 0) == WAIT_TIMEOUT, "other mask is still waiting");
  AwaitWake(view, 1);
  Reap(first, 0);

  HANDLE dead = Spawn(hardlink, 1, false), alive = Spawn(path, 0, true);
  AwaitReady(view, 4);
  Sleep(100);
  Require(TerminateProcess(dead, 88), "terminate one blocked child");
  Reap(dead, 88);
  AwaitWake(view, 0, true);
  Reap(alive, 0);
  Require(Futex(&view->word, FUTEX_WAKE, 1) == 0, "no remaining dead waiter");

  TestPshared(view, hardlink);
  Require(munmap(read_only, kPage) == 0 && munmap(view, kPage) == 0, "unmap aliases");
  Require(DeleteFileW(hardlink) && DeleteFileW(path), "delete backing file");
  puts("N29 production Windows mmap and futex process contract PASS");
  return 0;
}
