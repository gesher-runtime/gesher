#ifndef GESHER_SURFACE_FRAME_ACCOUNTING_H
#define GESHER_SURFACE_FRAME_ACCOUNTING_H
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace accounting {
// Every large allocation in these workloads is a production pixel vector.
// Fixture snapshots retain owners instead of copying image payloads.
constexpr size_t kPayloadMinimum = 4096;
struct alignas(std::max_align_t) Header { size_t bytes; };
std::atomic<size_t> live{0}, peak{0};
thread_local bool measuring = false;
thread_local size_t allocations = 0, bytes = 0, payload_allocations = 0, payload_bytes = 0;
void New(size_t amount) {
  if (measuring) {
    ++allocations;
    bytes += amount;
    if (amount >= kPayloadMinimum) { ++payload_allocations; payload_bytes += amount; }
  }
  if (amount >= kPayloadMinimum) {
    size_t now = live.fetch_add(amount) + amount;
    size_t before = peak.load();
    while (now > before && !peak.compare_exchange_weak(before, now)) {}
  }
}
void Start() {
  allocations = bytes = payload_allocations = payload_bytes = 0;
  peak = live.load();
  measuring = true;
}
}
void* operator new(size_t bytes) {
  auto* block = static_cast<accounting::Header*>(std::malloc(sizeof(accounting::Header) + bytes));
  if (block == nullptr) std::abort();
  block->bytes = bytes;
  accounting::New(bytes);
  return block + 1;
}
void operator delete(void* pointer) noexcept {
  if (pointer == nullptr) return;
  auto* block = static_cast<accounting::Header*>(pointer) - 1;
  if (block->bytes >= accounting::kPayloadMinimum) accounting::live.fetch_sub(block->bytes);
  std::free(block);
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, size_t) noexcept { ::operator delete(pointer); }

#endif
