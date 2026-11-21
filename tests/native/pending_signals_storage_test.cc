// SPDX-License-Identifier: Apache-2.0
// Isolated allocation/ownership test of the exact shared queue implementation.
// This is not evidence of Windows delivery, permissions, or per-UID accounting.
#include "gesher/pending_signals.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>

static size_t charged;
static size_t limit = 512;
static bool fail_allocation;
extern "C" bool gesher_signal_queue_acquire(bool enforce) {
  if (enforce && charged >= limit) return false;
  ++charged;
  return true;
}
extern "C" void gesher_signal_queue_release() { assert(charged != 0); --charged; }
extern "C" void* __real_malloc(size_t);
extern "C" void __real_free(void*);
extern "C" void* __wrap_malloc(size_t bytes) { return fail_allocation ? nullptr : __real_malloc(bytes); }
extern "C" void __wrap_free(void* ptr) { __real_free(ptr); }
struct Info { int value; char remaining[124]; };
int main() {
  using Queue = gesher::signals::Pending<Info>;
  Info input{}, output{};
  {
    Queue process, thread;
    input.value = 10;
    assert(process.Push(35, -1, input));
    input.value = 20;
    assert(process.Push(35, -1, input));
    assert(thread.Push(35, -1, input));
    assert(charged == 3);
    assert(process.Pop(35, &output) && output.value == 10);
    assert(thread.Pop(35, &output) && output.value == 20);
    assert(process.Pop(35, &output) && output.value == 20);
    assert(charged == 0);
    limit = 0;
    assert(!process.Push(35, -1, input));
    assert(process.Push(35, 0, input));
    assert(!process.Pop(35, &output) && output.value == 0);
    assert(charged == 0);
    limit = 512;
    fail_allocation = true;
    assert(!process.Push(35, -1, input));
    assert(process.Push(35, 0, input));
    assert(!process.Pop(35, &output));
    assert(process.Push(10, -1, input));
    assert(!process.Pop(10, &output));
    fail_allocation = false;
    input.value = 41;
    assert(process.Push(10, -1, input));
    input.value = 99;
    assert(process.Push(10, -1, input));
    assert(charged == 1);
    assert(process.Pop(10, &output) && output.value == 41);
    for (int i = 0; i < 512; ++i) assert(thread.Push(35, -1, input));
    assert(charged == 512);
    thread.Discard(35);
    assert(charged == 0 && thread.Mask() == 0);
    for (int i = 0; i < 40; ++i) assert(thread.Push(35, -1, input));
  }
  assert(charged == 0);  // Queue ownership releases pending records on retirement.
  auto started = std::chrono::steady_clock::now();
  for (int iteration = 0; iteration < 128; ++iteration) {
    Queue queue;
    for (int i = 0; i < 512; ++i) assert(queue.Push(35, -1, input));
    for (int i = 0; i < 512; ++i) assert(queue.Pop(35, &output));
  }
  auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started).count();
  assert(charged == 0);
  std::printf("PASS isolated shared queue: allocation/quota failure, kill fallback, FIFO, "
              "coalescing, flush, destruction; queue bytes=%zu; 65536 enqueue/dequeue pairs=%lld us\n",
              sizeof(Queue), static_cast<long long>(micros));
}
