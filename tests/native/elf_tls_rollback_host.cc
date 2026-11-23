// Exercises the complete production x86 TLS implementation on a host OS.
// Only the Win32 allocation/thread primitives below are substitutes. This
// does not verify Windows FLS, stack commitment, or rewritten guest code.
#include <windows.h>
#include "tls.h"

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

namespace elf = gesher::elf;
namespace {
constexpr size_t kPage = 4096;
constexpr size_t kStaticReserve = 1 << 20;

void Require(bool ok, const char* expression, int line) {
  if (!ok) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    std::abort();
  }
}
#define REQUIRE(expression) Require((expression), #expression, __LINE__)

struct Region { size_t size; std::vector<bool> committed; };
std::mutex platform_mutex;
std::map<uintptr_t, Region> regions;
std::unordered_map<void*, size_t> dynamic_allocations;
std::array<FlsCallback, 64> callbacks{};
size_t committed_bytes = 0;
int fail_commit_after = -1;
DWORD tls_count = 0;
DWORD fls_count = 0;
std::atomic<DWORD> next_thread_id{1};

// Pause an actual allocation call made while the production registry lock is
// held. Cached published TLS must remain usable on another existing thread.
struct CommitGate {
  std::mutex mutex;
  std::condition_variable condition;
  bool armed = false;
  bool entered = false;
  bool released = false;
} commit_gate;

struct ThreadState {
  DWORD id = next_thread_id.fetch_add(1);
  std::array<void*, 64> tls{};
  std::array<void*, 64> fls{};
  NT_TIB tib{};
  ~ThreadState() {
    for (size_t i = 0; i < fls.size(); ++i) {
      if (fls[i] != nullptr && callbacks[i] != nullptr) {
        void* value = fls[i];
        fls[i] = nullptr;
        callbacks[i](value);
      }
    }
  }
};
thread_local ThreadState thread_state;

size_t CommittedBytes() {
  std::lock_guard<std::mutex> lock(platform_mutex);
  return committed_bytes;
}
size_t DynamicCount() {
  std::lock_guard<std::mutex> lock(platform_mutex);
  return dynamic_allocations.size();
}
void FailCommitAfter(int successes) {
  std::lock_guard<std::mutex> lock(platform_mutex);
  fail_commit_after = successes;
}

// Keeps a real host thread (and production Arena) alive between operations.
class Worker {
 public:
  Worker() : thread_([this] {
    REQUIRE(elf::TlsSetupThread());
    std::unique_lock<std::mutex> lock(mutex_);
    complete_ = true;
    condition_.notify_all();
    for (;;) {
      condition_.wait(lock, [this] { return stop_ || static_cast<bool>(job_); });
      if (stop_) return;
      auto job = std::move(job_);
      job_ = {};
      lock.unlock();
      job();
      lock.lock();
      complete_ = true;
      condition_.notify_all();
    }
  }) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return complete_; });
  }
  ~Worker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
      condition_.notify_all();
    }
    thread_.join();
  }
  void Run(std::function<void()> job) {
    std::unique_lock<std::mutex> lock(mutex_);
    complete_ = false;
    job_ = std::move(job);
    condition_.notify_all();
    condition_.wait(lock, [this] { return complete_; });
  }
 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::function<void()> job_;
  bool complete_ = false;
  bool stop_ = false;
  std::thread thread_;
};

elf::TlsModule Module(const void* image, size_t init_size, size_t size, size_t align = 16) {
  elf::TlsModule result;
  result.present = true;
  result.init_image = image;
  result.init_size = init_size;
  result.total_size = size;
  result.align = align;
  return result;
}
uint8_t* Address(const elf::TlsModule& module) {
  return static_cast<uint8_t*>(elf::TlsGetAddr(module.module_id, 0));
}
void CheckImage(const elf::TlsModule& module) {
  uint8_t* block = Address(module);
  REQUIRE(block != nullptr);
  REQUIRE(std::memcmp(block, module.init_image, module.init_size) == 0);
  for (size_t i = module.init_size; i < module.total_size; ++i) REQUIRE(block[i] == 0);
}
void Pass(const char* name) { std::printf("PASS ELF TLS rollback: %s\n", name); }
}  // namespace

DWORD GetCurrentThreadId() { return thread_state.id; }
DWORD TlsAlloc() { return tls_count++; }
BOOL TlsFree(DWORD) { return TRUE; }
void* TlsGetValue(DWORD index) { return thread_state.tls.at(index); }
BOOL TlsSetValue(DWORD index, void* value) { thread_state.tls.at(index) = value; return TRUE; }
DWORD FlsAlloc(FlsCallback callback) {
  callbacks.at(fls_count) = callback;
  return fls_count++;
}
void* FlsGetValue(DWORD index) { return thread_state.fls.at(index); }
BOOL FlsSetValue(DWORD index, void* value) { thread_state.fls.at(index) = value; return TRUE; }
BOOL InitOnceExecuteOnce(PINIT_ONCE once, InitCallback callback, PVOID param, PVOID* context) {
  std::call_once(once->once, [&] { REQUIRE(callback(once, param, context)); });
  return TRUE;
}
void* VirtualAlloc(void* address, size_t size, DWORD type, DWORD) {
  if (type == MEM_COMMIT) {
    std::unique_lock<std::mutex> lock(commit_gate.mutex);
    if (commit_gate.armed) {
      commit_gate.armed = false;
      commit_gate.entered = true;
      commit_gate.condition.notify_all();
      commit_gate.condition.wait(lock, [] { return commit_gate.released; });
    }
  }
  std::lock_guard<std::mutex> lock(platform_mutex);
  REQUIRE(size != 0 && size % kPage == 0);
  if (type == MEM_RESERVE) {
    REQUIRE(address == nullptr);
    void* base = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return nullptr;
    regions.emplace(reinterpret_cast<uintptr_t>(base), Region{size, std::vector<bool>(size / kPage)});
    return base;
  }
  REQUIRE(type == MEM_COMMIT);
  if (fail_commit_after == 0) { fail_commit_after = -1; return nullptr; }
  if (fail_commit_after > 0) --fail_commit_after;
  uintptr_t base = reinterpret_cast<uintptr_t>(address);
  auto entry = regions.upper_bound(base);
  REQUIRE(entry != regions.begin());
  --entry;
  REQUIRE(base >= entry->first && base + size <= entry->first + entry->second.size);
  REQUIRE(base % kPage == 0);
  REQUIRE(mprotect(address, size, PROT_READ | PROT_WRITE) == 0);
  size_t start = (base - entry->first) / kPage;
  for (size_t i = start; i < start + size / kPage; ++i) {
    if (!entry->second.committed[i]) committed_bytes += kPage;
    entry->second.committed[i] = true;
  }
  return address;
}
BOOL VirtualFree(void* address, size_t size, DWORD type) {
  std::lock_guard<std::mutex> lock(platform_mutex);
  uintptr_t base = reinterpret_cast<uintptr_t>(address);
  auto entry = regions.upper_bound(base);
  REQUIRE(entry != regions.begin());
  --entry;
  if (type == MEM_RELEASE) {
    REQUIRE(base == entry->first && size == 0);
    committed_bytes -= kPage * std::count(entry->second.committed.begin(), entry->second.committed.end(), true);
    REQUIRE(munmap(address, entry->second.size) == 0);
    regions.erase(entry);
    return TRUE;
  }
  REQUIRE(type == MEM_DECOMMIT && size != 0 && size % kPage == 0);
  REQUIRE(base >= entry->first && base + size <= entry->first + entry->second.size);
  REQUIRE(mprotect(address, size, PROT_NONE) == 0);
  REQUIRE(madvise(address, size, MADV_DONTNEED) == 0);
  size_t start = (base - entry->first) / kPage;
  for (size_t i = start; i < start + size / kPage; ++i) {
    if (entry->second.committed[i]) committed_bytes -= kPage;
    entry->second.committed[i] = false;
  }
  return TRUE;
}
// Stack growth is outside this fixture: report no uncommitted stack range.
BOOL VirtualProtect(void*, size_t, DWORD, DWORD*) { REQUIRE(false); return 0; }
size_t VirtualQuery(void*, MEMORY_BASIC_INFORMATION*, size_t) { REQUIRE(false); return 0; }
void GetCurrentThreadStackLimits(ULONG_PTR* low, ULONG_PTR* high) { *low = *high = 0; }
void* NtCurrentTeb() { return &thread_state.tib; }
void* _aligned_malloc(size_t size, size_t align) {
  void* result = nullptr;
  if (posix_memalign(&result, align, size) != 0) return nullptr;
  std::lock_guard<std::mutex> lock(platform_mutex);
  dynamic_allocations.emplace(result, size);
  return result;
}
void _aligned_free(void* address) {
  std::lock_guard<std::mutex> lock(platform_mutex);
  REQUIRE(dynamic_allocations.erase(address) == 1);
  std::free(address);
}

int main() {
  alignas(64) const std::array<uint8_t, 16> image{0x21, 0x43, 0x65, 0x87};
  REQUIRE(elf::TlsSetupThread());
  Worker existing;
  const size_t empty_committed = CommittedBytes();
  REQUIRE(empty_committed == 2 * kPage);
  REQUIRE(!elf::TlsRollbackModule(nullptr));

  // A local-exec assignment cannot fall back to dynamic TLS after a partial
  // commit failure; both threads must return to their initial page budget.
  auto executable = Module(image.data(), image.size(), 2 * kPage);
  FailCommitAfter(1);
  REQUIRE(!elf::TlsAssignModule(&executable, true));
  REQUIRE(executable.module_id == 0 && !executable.is_static && !executable.ready);
  REQUIRE(CommittedBytes() == empty_committed);
  REQUIRE(!elf::TlsRollbackModule(&executable));
  REQUIRE(elf::TlsAssignModule(&executable, true));
  REQUIRE(executable.is_static && executable.tp_offset == -static_cast<int64_t>(2 * kPage));
  REQUIRE(elf::TlsRollbackModule(&executable));
  REQUIRE(CommittedBytes() == empty_committed);
  Pass("partial executable commit failure and retry restore every arena");

  auto stable = Module(image.data(), image.size(), 64);
  REQUIRE(elf::TlsAssignModule(&stable));
  const size_t stable_id = stable.module_id;
  existing.Run([&] { REQUIRE(Address(stable) == nullptr); });
  elf::TlsPublishModule(&stable);
  CheckImage(stable);
  Address(stable)[0] = 0xa1;
  existing.Run([&] { CheckImage(stable); Address(stable)[0] = 0xb2; });
  {
    Worker late;
    late.Run([&] { CheckImage(stable); });
  }
  const size_t stable_committed = CommittedBytes();
  REQUIRE(stable_committed == 4 * kPage);
  REQUIRE(!elf::TlsRollbackModule(&stable));
  REQUIRE(stable.module_id == stable_id && stable.ready);
  Pass("publication initializes existing/new threads and rejects committed rollback");

  auto pending = Module(image.data(), image.size(), 128);
  REQUIRE(elf::TlsAssignModule(&pending));
  size_t old_id = pending.module_id;
  auto* abandoned = Address(pending);
  REQUIRE(abandoned != nullptr && abandoned[0] == image[0]);
  abandoned[0] = 0xcc;
  REQUIRE(Address(pending) == abandoned);
  existing.Run([&] {
    REQUIRE(Address(pending) == nullptr);
    REQUIRE(!elf::TlsRollbackModule(&pending));
    REQUIRE(!elf::TlsAssignModule(&pending));
    elf::TlsPublishModule(&pending);
  });
  REQUIRE(!pending.ready && pending.module_id == old_id);
  // A controlled identity change keeps the owner's cached Arena: this verifies
  // that ownership is checked before the cached-address return as well.
  const DWORD owner_id = thread_state.id;
  thread_state.id = 0;
  REQUIRE(Address(pending) == nullptr);
  thread_state.id = owner_id;
  REQUIRE(elf::TlsRollbackModule(&pending));
  REQUIRE(!elf::TlsRollbackModule(&pending));
  REQUIRE(pending.module_id == 0 && pending.tp_offset == 0 && !pending.is_static && !pending.ready);
  REQUIRE(elf::TlsGetAddr(old_id, 0) == nullptr && abandoned[0] == 0);
  REQUIRE(Address(stable)[0] == 0xa1);
  existing.Run([&] { REQUIRE(Address(stable)[0] == 0xb2); });
  Pass("owned pending access, foreign/cache rejection, idempotent removal and shared-page clearing");

  auto dynamic = Module(image.data(), image.size(), 128, 2 * kPage);
  REQUIRE(elf::TlsAssignModule(&dynamic) && !dynamic.is_static);
  old_id = dynamic.module_id;
  CheckImage(dynamic);
  REQUIRE(DynamicCount() == 1);
  REQUIRE(elf::TlsGetAddr(old_id, 3) == Address(dynamic) + 3);
  existing.Run([&] { REQUIRE(Address(dynamic) == nullptr); });
  REQUIRE(elf::TlsRollbackModule(&dynamic));
  REQUIRE(DynamicCount() == 0 && elf::TlsGetAddr(old_id, 0) == nullptr);
  REQUIRE(elf::TlsAssignModule(&dynamic) && dynamic.module_id > old_id);
  REQUIRE(elf::TlsGetAddr(old_id, 0) == nullptr);
  CheckImage(dynamic);
  REQUIRE(elf::TlsRollbackModule(&dynamic) && DynamicCount() == 0);
  Pass("dynamic cache rollback frees its block and never reuses a stale module id");

  auto commit_failure = Module(image.data(), image.size(), 3 * kPage);
  FailCommitAfter(1);
  REQUIRE(elf::TlsAssignModule(&commit_failure) && !commit_failure.is_static);
  REQUIRE(CommittedBytes() == stable_committed);
  CheckImage(commit_failure);
  REQUIRE(elf::TlsRollbackModule(&commit_failure));
  REQUIRE(elf::TlsAssignModule(&commit_failure) && commit_failure.is_static);
  REQUIRE(elf::TlsRollbackModule(&commit_failure));
  REQUIRE(CommittedBytes() == stable_committed && DynamicCount() == 0);
  Pass("partial DSO commit failure trims pages before dynamic fallback and static retry");

  for (int retry = 0; retry < 32; ++retry) {
    auto full = Module(image.data(), image.size(), kStaticReserve - stable.total_size);
    auto overflow = Module(image.data(), image.size(), 64);
    REQUIRE(elf::TlsAssignModule(&full) && full.is_static);
    REQUIRE(elf::TlsAssignModule(&overflow) && !overflow.is_static);
    CheckImage(overflow);
    size_t full_id = full.module_id, overflow_id = overflow.module_id;
    REQUIRE(elf::TlsRollbackModule(&overflow));
    REQUIRE(elf::TlsRollbackModule(&full));
    REQUIRE(elf::TlsGetAddr(full_id, 0) == nullptr && elf::TlsGetAddr(overflow_id, 0) == nullptr);
    REQUIRE(CommittedBytes() == stable_committed && DynamicCount() == 0);
  }
  Pass("32 static-exhaustion/rollback retries preserve the static budget");

  auto* outer = new elf::TlsModule(Module(image.data(), image.size(), 3 * kPage));
  REQUIRE(elf::TlsAssignModule(outer) && outer->is_static);
  const size_t outer_id = outer->module_id;
  Address(*outer)[0] = 0xdd;
  auto nested = Module(image.data(), image.size(), 64);
  REQUIRE(elf::TlsAssignModule(&nested) && nested.is_static);
  elf::TlsPublishModule(&nested);
  const auto nested_offset = nested.tp_offset;
  uint8_t* nested_address = Address(nested);
  CheckImage(nested);
  nested_address[0] = 0xe3;
  existing.Run([&] { CheckImage(nested); Address(nested)[0] = 0xf4; });
  const size_t nested_committed = CommittedBytes();
  REQUIRE(elf::TlsRollbackModule(outer));
  delete outer;  // A stale registry pointer is now visible to ASan on later use.
  REQUIRE(elf::TlsGetAddr(outer_id, 0) == nullptr);
  REQUIRE(CommittedBytes() == nested_committed && nested.tp_offset == nested_offset);
  REQUIRE(Address(nested) == nested_address && nested_address[0] == 0xe3);
  existing.Run([&] { REQUIRE(Address(nested)[0] == 0xf4); });
  auto tail = Module(image.data(), image.size(), 2 * kPage);
  REQUIRE(elf::TlsAssignModule(&tail) && tail.is_static && tail.tp_offset > nested_offset);
  REQUIRE(elf::TlsRollbackModule(&tail));
  REQUIRE(CommittedBytes() == nested_committed);
  {
    Worker late;
    late.Run([&] {
      CheckImage(stable);
      CheckImage(nested);
      REQUIRE(elf::TlsGetAddr(outer_id, 0) == nullptr);
    });
  }
  REQUIRE(!elf::TlsRollbackModule(&nested) && nested.ready);
  REQUIRE(Address(stable)[0] == 0xa1);
  existing.Run([&] { REQUIRE(Address(stable)[0] == 0xb2); });
  Pass("newer committed nested library survives outer rollback and later thread setup");

  // Each nested success owns a fixed offset for its lifetime. Failed large
  // owners between them must remain reusable, including alignment padding.
  std::array<elf::TlsModule, 64> survivors;
  std::array<int64_t, 64> survivor_offsets{};
  for (size_t retry = 0; retry < survivors.size(); ++retry) {
    auto failed = Module(image.data(), image.size(), 64 * kPage,
                         retry % 2 ? 64 : kPage);
    REQUIRE(elf::TlsAssignModule(&failed) && failed.is_static);
    const size_t retired_id = failed.module_id;
    std::memset(Address(failed), 0xcc, failed.total_size);
    survivors[retry] = Module(image.data(), image.size(), 64, 64);
    auto& survivor = survivors[retry];
    REQUIRE(elf::TlsAssignModule(&survivor) && survivor.is_static);
    survivor_offsets[retry] = survivor.tp_offset;
    elf::TlsPublishModule(&survivor);
    CheckImage(survivor);
    Address(survivor)[0] = 0x6a;
    existing.Run([&] { CheckImage(survivor); Address(survivor)[0] = 0x7b; });
    REQUIRE(elf::TlsRollbackModule(&failed));
    REQUIRE(elf::TlsGetAddr(retired_id, 0) == nullptr);
    // Two live arenas, with less than 16 KiB of committed module data each.
    // A single surviving offset can retain a high-water page, but 64 failed
    // 256 KiB reservations must not grow that high water on every attempt.
    REQUIRE(CommittedBytes() <= 2 * (80 * kPage));
    for (size_t i = 0; i <= retry; ++i) {
      REQUIRE(survivors[i].tp_offset == survivor_offsets[i]);
      REQUIRE(Address(survivors[i])[0] == 0x6a);
      existing.Run([&] { REQUIRE(Address(survivors[i])[0] == 0x7b); });
    }
  }
  {
    Worker late;
    late.Run([&] { for (const auto& survivor : survivors) CheckImage(survivor); });
  }
  Pass("64 nested commits reuse failed interior extents without moving live TLS");

  const int initial_tls = 37;
  auto prepared = Module(&initial_tls, sizeof(initial_tls), 64);
  REQUIRE(elf::TlsAssignModule(&prepared));
  existing.Run([&] {
    REQUIRE(!elf::TlsPrepareModule(&prepared));
    REQUIRE(Address(prepared) == nullptr);
  });
  REQUIRE(elf::TlsPrepareModule(&prepared) && !prepared.ready);
  auto* direct = reinterpret_cast<int*>(elf::TlsCurrentThreadPointer() + prepared.tp_offset);
  REQUIRE(*direct == 37);  // Initial-exec access does not call TlsGetAddr.
  *direct = 93;
  REQUIRE(elf::TlsPrepareModule(&prepared) && *direct == 93);
  existing.Run([&] {
    REQUIRE(*reinterpret_cast<int*>(elf::TlsCurrentThreadPointer() + prepared.tp_offset) == 0);
    REQUIRE(Address(prepared) == nullptr);
  });
  elf::TlsPublishModule(&prepared);
  REQUIRE(*direct == 93 && *reinterpret_cast<int*>(Address(prepared)) == 93);
  existing.Run([&] { REQUIRE(*reinterpret_cast<int*>(Address(prepared)) == 37); });
  {
    Worker late;
    late.Run([&] { REQUIRE(*reinterpret_cast<int*>(Address(prepared)) == 37); });
  }
  auto abandoned_prepared = Module(&initial_tls, sizeof(initial_tls), 64);
  REQUIRE(elf::TlsAssignModule(&abandoned_prepared));
  REQUIRE(elf::TlsPrepareModule(&abandoned_prepared));
  const size_t abandoned_prepared_id = abandoned_prepared.module_id;
  REQUIRE(elf::TlsRollbackModule(&abandoned_prepared));
  REQUIRE(elf::TlsGetAddr(abandoned_prepared_id, 0) == nullptr && *direct == 93);
  auto delayed_arena = Module(&initial_tls, sizeof(initial_tls), 64);
  std::thread delayed_owner([&] {
    REQUIRE(elf::TlsAssignModule(&delayed_arena));
    REQUIRE(elf::TlsPrepareModule(&delayed_arena));
    REQUIRE(elf::TlsSetupThread());
    auto* value = reinterpret_cast<int*>(elf::TlsCurrentThreadPointer() + delayed_arena.tp_offset);
    REQUIRE(*value == 37);
    *value = 93;
    elf::TlsPublishModule(&delayed_arena);
    REQUIRE(*value == 93);
  });
  delayed_owner.join();
  REQUIRE(*reinterpret_cast<int*>(Address(delayed_arena)) == 37);
  Pass("private relocated images preserve owner writes, exclude foreign threads, and remain rollbackable");

  // A published dynamic block must survive unrelated failed assignments too.
  REQUIRE(elf::TlsAssignModule(&dynamic));
  elf::TlsPublishModule(&dynamic);
  CheckImage(dynamic);
  Address(dynamic)[0] = 0x5a;
  existing.Run([&] { CheckImage(dynamic); Address(dynamic)[0] = 0x6b; });
  REQUIRE(DynamicCount() == 2 && !elf::TlsRollbackModule(&dynamic));
  auto failed_dynamic = Module(image.data(), image.size(), 64, 2 * kPage);
  REQUIRE(elf::TlsAssignModule(&failed_dynamic));
  CheckImage(failed_dynamic);
  REQUIRE(DynamicCount() == 3 && elf::TlsRollbackModule(&failed_dynamic));
  REQUIRE(DynamicCount() == 2 && Address(dynamic)[0] == 0x5a);
  existing.Run([&] { REQUIRE(Address(dynamic)[0] == 0x6b); });
  Pass("published dynamic TLS retains per-thread state across unrelated rollback");

  {
    std::mutex completed_mutex;
    std::condition_variable completed_condition;
    bool completed = false;
    {
      std::lock_guard<std::mutex> lock(commit_gate.mutex);
      commit_gate.armed = true;
      commit_gate.entered = commit_gate.released = false;
    }
    std::thread allocator([&] {
      auto blocked = Module(image.data(), image.size(), kStaticReserve / 2);
      REQUIRE(elf::TlsAssignModule(&blocked) && blocked.is_static);
      REQUIRE(elf::TlsRollbackModule(&blocked));
    });
    bool entered;
    {
      std::unique_lock<std::mutex> lock(commit_gate.mutex);
      entered = commit_gate.condition.wait_for(lock, std::chrono::seconds(5),
                                               [] { return commit_gate.entered; });
    }
    std::thread lookup([&] {
      existing.Run([&] {
        REQUIRE(Address(stable)[0] == 0xb2);
        REQUIRE(Address(dynamic)[0] == 0x6b);
        REQUIRE(elf::TlsGetAddr(stable.module_id, 3) == Address(stable) + 3);
      });
      std::lock_guard<std::mutex> lock(completed_mutex);
      completed = true;
      completed_condition.notify_all();
    });
    bool independent;
    {
      std::unique_lock<std::mutex> lock(completed_mutex);
      independent = completed_condition.wait_for(lock, std::chrono::seconds(5),
                                                  [&] { return completed; });
    }
    // Always release and join, including the old-source negative control.
    {
      std::lock_guard<std::mutex> lock(commit_gate.mutex);
      commit_gate.armed = false;
      commit_gate.released = true;
      commit_gate.condition.notify_all();
    }
    lookup.join();
    allocator.join();
    REQUIRE(entered && independent);
  }
  Pass("cached published static/dynamic lookup does not wait for unrelated registry work");

  {
    static std::array<elf::TlsModule, 32> concurrent_survivors;
    std::atomic<bool> running{false}, stop{false};
    size_t reads = 0;
    std::thread reader([&] {
      REQUIRE(elf::TlsSetupThread());
      CheckImage(stable);
      CheckImage(dynamic);
      uint8_t* static_address = Address(stable);
      uint8_t* dynamic_address = Address(dynamic);
      static_address[0] = 0x91;
      dynamic_address[0] = 0x82;
      running.store(true);
      running.notify_one();
      do {
        REQUIRE(Address(stable) == static_address && static_address[0] == 0x91);
        REQUIRE(Address(dynamic) == dynamic_address && dynamic_address[0] == 0x82);
        ++reads;
      } while (!stop.load());
    });
    running.wait(false);
    for (size_t i = 0; i < concurrent_survivors.size(); ++i) {
      auto failed = Module(image.data(), image.size(), 64, i % 2 ? 2 * kPage : 16);
      REQUIRE(elf::TlsAssignModule(&failed));
      CheckImage(failed);
      const size_t retired_id = failed.module_id;
      auto& survivor = concurrent_survivors[i];
      survivor = Module(image.data(), image.size(), 64);
      REQUIRE(elf::TlsAssignModule(&survivor));
      elf::TlsPublishModule(&survivor);
      REQUIRE(elf::TlsRollbackModule(&failed));
      REQUIRE(elf::TlsGetAddr(retired_id, 0) == nullptr);
      Worker transient;
      transient.Run([&] { CheckImage(survivor); });
    }
    stop.store(true);
    reader.join();
    REQUIRE(reads > 0);
  }
  Pass("published cache survives concurrent rollback, publication and thread teardown");
  std::puts("ELF TLS rollback host contracts passed (complete production tls.cc; simulated Win32 boundary)");
}
