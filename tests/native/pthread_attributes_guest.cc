// Public NDK pthread contracts, executed as both x86_64 and arm64 ELF guests.
// Can also run on Linux as a positive reference for the shared POSIX subset.
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* label) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s guest pthread attributes: %s\n", ok ? "PASS" : "FAIL", label);
  std::fflush(stdout);
}
[[noreturn]] void Abort(const char* reason) {
  Check(false, reason);
  std::_Exit(1);  // A live worker may still reference caller-owned state.
}
bool Contains(void* base, size_t size, uintptr_t address) {
  uintptr_t low = reinterpret_cast<uintptr_t>(base);
  return address >= low && address - low < size;
}
#if defined(__BIONIC__)
bool DescriptorPageReadable(pthread_t thread) {
  uintptr_t address = static_cast<uintptr_t>(thread);
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return false;
  uintptr_t size = static_cast<uintptr_t>(page_size);
  if (address < 0x10000 || (address % sizeof(void*)) != 0) return false;
  uintptr_t page_end = (address & ~(size - 1)) + size;
  for (uintptr_t cursor = address; cursor < page_end; cursor += sizeof(uintptr_t)) {
    volatile const uintptr_t* slot = reinterpret_cast<const volatile uintptr_t*>(cursor);
    (void)*slot;
  }
  return true;
}
// pthread_internal_t: next and prev links, then tid and the 31-bit cached
// pid. Call only after DescriptorPageReadable() accepted the address.
bool DescriptorFieldsMatch(pthread_t thread) {
  uintptr_t address = static_cast<uintptr_t>(thread);
  uintptr_t fields = address + 2 * sizeof(void*);
  int32_t tid = *reinterpret_cast<const volatile int32_t*>(fields);
  uint32_t pid = *reinterpret_cast<const volatile uint32_t*>(fields + sizeof(int32_t));
  return tid == gettid() && static_cast<pid_t>(pid & 0x7fffffffu) == getpid();
}
#endif
struct Worker {
  std::atomic<bool> ready{false}, stop{false}, leaving{false};
  void* base = nullptr;
  size_t size = 0;
  uintptr_t local = 0;
  int result = -1;
#if defined(__BIONIC__)
  pthread_t self = 0;
  bool self_readable = false;
  bool self_fields = false;
#endif
};
void* Run(void* raw) {
  auto& worker = *static_cast<Worker*>(raw);
  int local = 0;
  pthread_attr_t self{};
  pthread_t current = pthread_self();
  worker.local = reinterpret_cast<uintptr_t>(&local);
#if defined(__BIONIC__)
  worker.self = current;
  worker.self_readable = DescriptorPageReadable(current);
  worker.self_fields = worker.self_readable && DescriptorFieldsMatch(current);
#endif
  worker.result = pthread_getattr_np(current, &self);
  if (worker.result == 0) {
    worker.result = pthread_attr_getstack(&self, &worker.base, &worker.size);
    pthread_attr_destroy(&self);
  }
  worker.ready.store(true);
  while (!worker.stop.load()) usleep(1000);
  worker.leaving.store(true);
  return reinterpret_cast<void*>(0x1234);
}
void Ready(Worker& worker) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!worker.ready.load()) {
    if (std::chrono::steady_clock::now() >= deadline) Abort("target startup deadline");
    usleep(1000);
  }
}
}
int main() {
  pthread_attr_t attr{};
  Check(pthread_attr_init(&attr) == 0, "initialize public attributes");
  int value = -1;
  Check(pthread_attr_getinheritsched(&attr, &value) == 0 && value == PTHREAD_INHERIT_SCHED,
        "public default inherits scheduling");
  Check(pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0 &&
            pthread_attr_getinheritsched(&attr, &value) == 0 && value == PTHREAD_EXPLICIT_SCHED,
        "explicit inheritance roundtrip");
  Check(pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED) == 0 &&
            pthread_attr_getinheritsched(&attr, &value) == 0 && value == PTHREAD_INHERIT_SCHED,
        "inherited scheduling roundtrip");
  Check(pthread_attr_setinheritsched(&attr, -1) == EINVAL &&
            pthread_attr_setinheritsched(&attr, 2) == EINVAL,
        "invalid inheritance is rejected");
  Check(pthread_attr_setdetachstate(&attr, -1) == EINVAL &&
            pthread_attr_setdetachstate(&attr, 2) == EINVAL,
        "invalid detach state is rejected");
  Check(pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) == 0 &&
            pthread_attr_setscope(&attr, PTHREAD_SCOPE_PROCESS) == ENOTSUP &&
            pthread_attr_setscope(&attr, -1) == EINVAL,
        "public scope validation");
#if defined(__BIONIC__)
  // Android requires page-aligned supplied stacks; glibc does not promise it.
  Check(pthread_attr_setstack(&attr, reinterpret_cast<void*>(0x100001), 16384) == EINVAL &&
            pthread_attr_setstack(&attr, reinterpret_cast<void*>(0x100000), 16385) == EINVAL,
        "Android supplied stack alignment");
  Check(DescriptorPageReadable(pthread_self()) && DescriptorFieldsMatch(pthread_self()),
        "main thread descriptor is readable with Bionic tid and pid");
#endif
  pthread_attr_destroy(&attr);
  int caller = 0;
  for (int kind = 0; kind < 3; ++kind) {
    Worker worker;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, kind == 1 ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
    pthread_t target{};
    if (pthread_create(&target, &attr, Run, &worker) != 0) Abort("create public pthread target");
    pthread_attr_destroy(&attr);
    pthread_attr_t peer{};
    Check(pthread_getattr_np(target, &peer) == 0, "public peer query immediately after creation");
    void* base = nullptr;
    size_t size = 0;
    Check(pthread_attr_getstack(&peer, &base, &size) == 0, "public peer stack snapshot");
    pthread_attr_destroy(&peer);
    Ready(worker);
    Check(worker.result == 0 && worker.base == base && worker.size == size,
          "peer and target self report same bounds");
    Check(Contains(base, size, worker.local) &&
              !Contains(base, size, reinterpret_cast<uintptr_t>(&caller)),
          "peer contains target local and excludes caller local");
#if defined(__BIONIC__)
    Check(pthread_equal(worker.self, target) != 0 && worker.self_readable,
          "worker pthread_self matches peer handle and descriptor page is readable");
    Check(worker.self_fields, "worker descriptor holds gettid() and getpid() at Bionic offsets");
#endif
    if (kind == 2) Check(pthread_detach(target) == 0, "public detach running target");
    Check(pthread_getattr_np(target, &peer) == 0 &&
              pthread_attr_getdetachstate(&peer, &value) == 0 &&
              value == (kind == 0 ? PTHREAD_CREATE_JOINABLE : PTHREAD_CREATE_DETACHED),
          "public snapshot observes current detach state");
    pthread_attr_destroy(&peer);
    worker.stop.store(true);
    if (kind == 0) {
      void* result = nullptr;
      Check(pthread_join(target, &result) == 0 && result == reinterpret_cast<void*>(0x1234),
            "public join preserves callback result");
    } else {
      // Detached callbacks stop touching the state before this release/acquire
      // handshake; joining a detached thread is deliberately not attempted.
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!worker.leaving.load()) {
        if (std::chrono::steady_clock::now() >= deadline) Abort("detached callback deadline");
        usleep(1000);
      }
    }
  }
  std::printf("guest pthread attributes: %d checks, %d failures\n", checks, failures);
  std::puts(failures == 0 ? "PASS guest pthread attributes" : "FAIL guest pthread attributes");
  return failures == 0 ? 0 : 1;
}
