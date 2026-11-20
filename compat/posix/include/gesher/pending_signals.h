// SPDX-License-Identifier: Apache-2.0
// Pending signal storage shared by the host POSIX and loaded bionic adapters.
// Queue ownership/locking belongs to the caller. The allocator never throws.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>

extern "C" bool gesher_signal_queue_acquire(bool enforce_limit);
extern "C" void gesher_signal_queue_release();

namespace gesher::signals {

template <class Info>
class Pending {
 public:
  Pending() = default;
  Pending(const Pending&) = delete;
  Pending& operator=(const Pending&) = delete;
  ~Pending() { Clear(); }

  uint64_t Mask() const { return mask_; }

  // Standard signals coalesce, preserving the first record. Real-time
  // signals retain one record per send, FIFO within each signal number.
  // Linux may lose siginfo when kill() cannot allocate; the pending bit
  // still guarantees a delivery. sigqueue(RT) must instead fail with EAGAIN.
  bool Push(int sig, int code, const Info& info) {
    static_assert(std::is_trivially_copyable<Info>::value, "signal record");
    const uint64_t bit = uint64_t{1} << (sig - 1);
    if (sig < 32 && (mask_ & bit) != 0) return true;
    void* memory = std::malloc(sizeof(Node));
    Node* node = nullptr;
    if (memory != nullptr) {
      // SI_USER standard signals may exceed the queue limit, as Linux's
      // legacy kill path does. RT kill still attempts quota-accounted FIFO.
      if (gesher_signal_queue_acquire(sig >= 32 || code < 0)) {
        node = new (memory) Node{info, nullptr};
      } else {
        std::free(memory);
      }
    }
    if (node == nullptr) {
      if (sig >= 32 && code != 0 /* SI_USER */) return false;
      mask_ |= bit;
      return true;
    }
    if (tails_[sig] != nullptr) tails_[sig]->next = node;
    else heads_[sig] = node;
    tails_[sig] = node;
    mask_ |= bit;
    return true;
  }

  // Returns whether a siginfo record survived allocation. A false result
  // on a pending signal means the caller supplies Linux's zeroed SI_USER
  // fallback, with only si_signo set. It does not fabricate sender data.
  bool Pop(int sig, Info* info) {
    Node* node = heads_[sig];
    if (node != nullptr) {
      if (info != nullptr) *info = node->info;
      heads_[sig] = node->next;
      node->~Node();
      std::free(node);
      gesher_signal_queue_release();
    } else if (info != nullptr) {
      *info = {};
    }
    if (heads_[sig] == nullptr) {
      tails_[sig] = nullptr;
      mask_ &= ~(uint64_t{1} << (sig - 1));
    }
    return node != nullptr;
  }

  void Discard(int sig) {
    while (heads_[sig] != nullptr) Pop(sig, nullptr);
    mask_ &= ~(uint64_t{1} << (sig - 1));
  }

  void Clear() {
    for (int sig = 1; sig <= 64; ++sig) Discard(sig);
  }

 private:
  struct Node { Info info; Node* next; };
  Node* heads_[65] = {};
  Node* tails_[65] = {};
  uint64_t mask_ = 0;
};

}  // namespace gesher::signals
