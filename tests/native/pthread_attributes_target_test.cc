// Real Windows integration: peer guest/external stacks have independently
// sampled Win32 intervals and owned native completion witnesses. Link against
// gesher_bionic + gesher_elf; do not substitute the host-source boundary here.
#include "pthread_attributes_test_api.h"
#include <gesher/bionic_shim.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace pthread_attributes_test;
namespace {
Api api;
struct Worker {
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  Thread id = 0;
  Attr own{};
  int own_result = -1;
  uintptr_t local = 0;
  ULONG_PTR low = 0, high = 0;
  HANDLE witness = nullptr;
};
[[noreturn]] void AbortFixture(const char* reason) {
  Check(false, reason);
  TerminateProcess(GetCurrentProcess(), 1);
  std::_Exit(1);  // Do not destroy state that an unfinished worker still owns.
}
void Ready(Worker& worker) {
  ULONGLONG deadline = GetTickCount64() + 10000;
  while (!worker.ready.load()) {
    if (GetTickCount64() >= deadline) AbortFixture("target publication deadline");
    Sleep(1);
  }
}
void* BIONIC_SYSV RunGuest(void* raw) {
  auto& worker = *static_cast<Worker*>(raw);
  int local = 0;
  GetCurrentThreadStackLimits(&worker.low, &worker.high);
  worker.local = reinterpret_cast<uintptr_t>(&local);
  worker.id = api.self();
  worker.own_result = api.getattr(worker.id, &worker.own);
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &worker.witness,
                  SYNCHRONIZE, FALSE, 0);
  worker.ready.store(true);
  while (!worker.stop.load()) Sleep(1);
  return reinterpret_cast<void*>(0x1234);
}
DWORD WINAPI RunExternal(void* raw) { RunGuest(raw); return 0; }
void Finish(Worker& worker) {
  if (!worker.witness || WaitForSingleObject(worker.witness, 10000) != WAIT_OBJECT_0)
    AbortFixture("owned target witness confirms termination");
  CloseHandle(worker.witness);
  worker.witness = nullptr;
}
bool IsDescriptorReadable(Thread id) {
  uintptr_t address = static_cast<uintptr_t>(id);
  if (address < 0x10000 || (address % sizeof(void*)) != 0) return false;
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0) return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0) return false;
  constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                              PAGE_EXECUTE_WRITECOPY;
  if ((mbi.Protect & kReadable) == 0) return false;
  uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  uintptr_t page_end = (address & ~uintptr_t{4095}) + 4096;
  if (page_end - region_start > mbi.RegionSize) return false;
  for (uintptr_t cursor = address; cursor < page_end; cursor += sizeof(uintptr_t)) {
    volatile const uintptr_t* slot = reinterpret_cast<const volatile uintptr_t*>(cursor);
    (void)*slot;
  }
  return true;
}
void Verify(Thread id, Worker& worker, uintptr_t caller) {
  Check(IsDescriptorReadable(id), "target descriptor stays page-readable");
  Attr attr{};
  Check(api.getattr(id, &attr) == 0 &&
            reinterpret_cast<ULONG_PTR>(attr.stack_base) == worker.low &&
            attr.stack_size == worker.high - worker.low,
        "peer snapshot equals independently observed Win32 bounds");
  Check(Contains(attr, worker.local) && !Contains(attr, caller),
        "peer interval contains its local and excludes caller local");
  Check(worker.own_result == 0 && worker.own.stack_base == attr.stack_base &&
            worker.own.stack_size == attr.stack_size,
        "self and peer agree on actual target stack");
  DWORD before = 0, after = 0;
  bool counted = GetProcessHandleCount(GetCurrentProcess(), &before) != 0;
  bool repeated = true;
  for (int i = 0; i < 512; ++i) {
    Attr sample{};
    repeated &= api.getattr(id, &sample) == 0 && sample.stack_base == attr.stack_base &&
                sample.stack_size == attr.stack_size;
  }
  counted &= GetProcessHandleCount(GetCurrentProcess(), &after) != 0;
  Check(repeated && counted && before == after,
        "512 peer queries preserve bounds and allocate no handles");
}
void Retired(Thread id) {
  Attr attr;
  std::memset(&attr, 0x5a, sizeof attr);
  Attr before = attr;
  Check(api.getattr(id, &attr) == 3 && std::memcmp(&attr, &before, sizeof attr) == 0,
        "retired target is rejected without output mutation");
}
}
int main() {
  if (!Load(api, gesher::bionic::LookupBionicSymbol)) return 1;
  ValueContracts(api);
  int local = 0;
  Attr self{};
  Check(api.getattr(api.self(), &self) == 0 && Contains(self, reinterpret_cast<uintptr_t>(&local)),
        "calling external thread reports its actual stack");
  Check(IsDescriptorReadable(api.self()), "caller self descriptor is page-readable");
  for (int kind : {0, 1, 2}) {
    Worker worker;
    Attr requested{};
    api.init(&requested);
    api.size_set(&requested, 2 * 1024 * 1024);
    api.detach_set(&requested, kind == 1 ? 1 : 0);
    Thread target = 0;
    if (api.create(&target, &requested, RunGuest, &worker) != 0)
      AbortFixture("create guest target");
    Attr early{};
    Check(api.getattr(target, &early) == 0, "query directly after pthread_create");
    Ready(worker);
    Verify(target, worker, reinterpret_cast<uintptr_t>(&local));
    Check(early.stack_base == worker.own.stack_base && early.stack_size == worker.own.stack_size,
          "early query receives published target bounds");
    if (kind == 2) Check(api.detach(target) == 0, "detach running guest target");
    Attr mode{};
    int detached = -1;
    Check(api.getattr(target, &mode) == 0 && api.detach_get(&mode, &detached) == 0 &&
              detached == (kind == 0 ? 0 : 1), "runtime detach state matches current guest state");
    worker.stop.store(true);
    if (kind == 0) {
      void* result = nullptr;
      Check(api.join(target, &result) == 0 && result == reinterpret_cast<void*>(0x1234),
            "join retains user callback result");
    }
    Finish(worker);
    Retired(target);
  }
  Worker external;
  HANDLE native = CreateThread(nullptr, 3 * 1024 * 1024, RunExternal, &external,
                               STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!native) AbortFixture("create external target");
  Ready(external);
  Verify(external.id, external, reinterpret_cast<uintptr_t>(&local));
  external.stop.store(true);
  Finish(external);
  CloseHandle(native);
  Retired(external.id);
  Retired(UINT64_MAX);
  Check(api.getattr(api.self(), nullptr) == 22, "null query output is rejected");
  std::vector<Thread> seen_ids{api.self()};
  bool replacements = true;
  for (int i = 0; i < 32; ++i) {
    Worker replacement;
    Thread id = 0;
    if (api.create(&id, nullptr, RunGuest, &replacement) != 0)
      AbortFixture("create replacement target");
    replacements &= IsDescriptorReadable(id) &&
                    std::find(seen_ids.begin(), seen_ids.end(), id) == seen_ids.end();
    seen_ids.push_back(id);
    Attr snapshot{};
    replacements &= api.getattr(id, &snapshot) == 0;
    Ready(replacement);
    replacements &= snapshot.stack_base == replacement.own.stack_base &&
                    snapshot.stack_size == replacement.own.stack_size;
    replacement.stop.store(true);
    replacements &= api.join(id, nullptr) == 0;
    Finish(replacement);
    replacements &= api.getattr(id, &snapshot) == 3 && IsDescriptorReadable(id);
  }
  Check(replacements && seen_ids.size() == 33,
        "32 replacement threads keep unique page-readable identities across retirement");
  std::printf("pthread attributes: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
