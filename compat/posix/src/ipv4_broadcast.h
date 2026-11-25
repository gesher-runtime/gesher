// Classification of a received IPv4 datagram's destination for IP_PKTINFO's
// ipi_spec_dst (winsock.cc), kept free of Windows headers so that
// tests/native/udp_pktinfo_test.cc can check it directly with a synthetic
// interface list.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gesher::net {

// One local IPv4 address and its on-link mask (host byte order).
struct Ipv4Interface {
  uint32_t address = 0, mask = 0;
};

// The directed-broadcast address of a subnet (address | ~mask), or 0 for a
// /31 or /32, which has none (RFC 3021).
constexpr uint32_t DirectedBroadcast(uint32_t address, uint32_t mask) {
  return mask < 0xfffffffeu ? (address | ~mask) : 0;
}

// The local subnets' directed-broadcast addresses, read without a lock on
// every receive: a seqlock over a fixed array. Publish has one writer at a
// time (its caller serializes it); a reader that races it retries, and after
// a bounded number of attempts answers kUnknown rather than spinning.
class DirectedBroadcastSet {
 public:
  static constexpr size_t kCapacity = 64;
  enum class Match { kNo, kYes, kUnknown };

  // Replaces the set with `interfaces`' broadcast addresses. More interfaces
  // than kCapacity leave it incomplete: every query is then kUnknown.
  void Publish(const Ipv4Interface* interfaces, size_t count) {
    BeginWrite();
    size_t stored = 0;
    bool complete = true;
    for (size_t i = 0; i < count; ++i) {
      const uint32_t broadcast = DirectedBroadcast(interfaces[i].address, interfaces[i].mask);
      if (broadcast == 0) continue;
      if (stored == kCapacity) {
        complete = false;
        break;
      }
      addresses_[stored++].store(broadcast, std::memory_order_relaxed);
    }
    count_.store(static_cast<uint32_t>(stored), std::memory_order_relaxed);
    complete_.store(complete, std::memory_order_relaxed);
    EndWrite();
  }

  // Forgets the interfaces (for example when they could not be read).
  void Invalidate() {
    BeginWrite();
    count_.store(0, std::memory_order_relaxed);
    complete_.store(false, std::memory_order_relaxed);
    EndWrite();
  }

  Match Contains(uint32_t destination) const {
    for (int attempt = 0; attempt < 64; ++attempt) {
      const uint32_t before = sequence_.load(std::memory_order_acquire);
      if ((before & 1) != 0) continue;
      const bool complete = complete_.load(std::memory_order_relaxed);
      uint32_t count = count_.load(std::memory_order_relaxed);
      if (count > kCapacity) count = kCapacity;
      bool found = false;
      for (uint32_t i = 0; i < count && !found; ++i) {
        found = addresses_[i].load(std::memory_order_relaxed) == destination;
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      if (sequence_.load(std::memory_order_relaxed) != before) continue;
      if (found) return Match::kYes;
      return complete ? Match::kNo : Match::kUnknown;
    }
    return Match::kUnknown;
  }

 private:
  void BeginWrite() {
    sequence_.store(sequence_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
  }
  void EndWrite() { sequence_.fetch_add(1, std::memory_order_release); }

  std::atomic<uint32_t> sequence_{0};
  std::atomic<uint32_t> count_{0};
  std::atomic<bool> complete_{false};  // never published: every query kUnknown
  std::atomic<uint32_t> addresses_[kCapacity] = {};
};

enum class Ipv4DestinationKind { kUnicast, kBroadcast, kMulticast, kUnknown };

// What a received IPv4 datagram's destination is. Windows marks a datagram
// received as a link-layer broadcast (MSG_BCAST) or multicast (MSG_MCAST),
// but a subnet-directed broadcast need not carry MSG_BCAST (one this host
// sent to itself never crosses a link), so a destination equal to a local
// subnet's broadcast address is a broadcast as well. kUnknown: the set could
// not answer, and the caller must consult the full interface list.
inline Ipv4DestinationKind ClassifyIpv4Destination(uint32_t destination, bool received_as_broadcast,
                                                   bool received_as_multicast,
                                                   const DirectedBroadcastSet& broadcasts) {
  if ((destination >> 28) == 0xe || received_as_multicast) return Ipv4DestinationKind::kMulticast;
  if (destination == 0xffffffffu || received_as_broadcast) return Ipv4DestinationKind::kBroadcast;
  switch (broadcasts.Contains(destination)) {
    case DirectedBroadcastSet::Match::kYes:
      return Ipv4DestinationKind::kBroadcast;
    case DirectedBroadcastSet::Match::kNo:
      return Ipv4DestinationKind::kUnicast;
    case DirectedBroadcastSet::Match::kUnknown:
      break;
  }
  return Ipv4DestinationKind::kUnknown;
}

}  // namespace gesher::net
