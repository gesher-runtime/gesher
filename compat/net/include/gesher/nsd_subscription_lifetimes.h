// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace gesher::nsd {

// The notification owner changes the generation before and after its bounded
// observation. A snapshot must see the same generation and no callback at both
// ends. In particular, a callback waiting for the state mutex is still in flight.
class SubscriptionSnapshotFence {
 public:
  void begin(std::atomic<uint64_t>& generation) {
    active_.fetch_add(1);
    generation.fetch_add(1);
  }
  void end(std::atomic<uint64_t>& generation) {
    generation.fetch_add(1);
    active_.fetch_sub(1);
  }
  bool stable(const std::atomic<uint64_t>& generation, uint64_t expected) const {
    return active_.load() == 0 && generation.load() == expected;
  }
 private:
  std::atomic<size_t> active_{0};
};

// Request-owned continuity observations, not Android Network identifiers. The
// native owner serializes access with its notification-state mutex. Socket I/O,
// JNI and notification cancellation never run under that mutex.
class SubscriptionLifetimes {
 public:
  static constexpr size_t kMaxScopes = 1024;
  static constexpr size_t kMaxAddressesPerScope = 64;
  static constexpr size_t kMaxAddresses = 4096;
  static constexpr unsigned kIpv4 = 1, kIpv6 = 2;
  struct Address {
    unsigned family = 0;
    std::array<unsigned char, 16> bytes{};
    bool operator==(const Address&) const = default;
  };
  struct Scope {
    uint64_t luid = 0;
    uint32_t index = 0, ipv4_index = 0, ipv6_index = 0;
    unsigned families = 0;
    std::vector<Address> addresses;
  };
  enum class Result { Ready, Retry, Unavailable, Capacity, Exhausted };

  bool contains(uint64_t luid) const { return scopes_.contains(luid); }
  size_t size() const { return scopes_.size(); }
  size_t address_count() const { return address_count_; }

  // Initial/null notifications carry no departure evidence and do not call
  // these methods. Only complete IP Helper queries or explicit deletion
  // notifications may provide the family-presence observation.
  void interface_changed(uint64_t luid, unsigned family, uint32_t index, bool present) {
    auto found = scopes_.find(luid);
    if (found == scopes_.end()) return;
    if (!valid_family(family) || !index) { unavailable_ = true; return; }
    auto& value = found->second;
    uint32_t& old_index = family == kIpv4 ? value.scope.ipv4_index : value.scope.ipv6_index;
    if (old_index && old_index != index) {
      // A delayed deletion for an old family index cannot delete its observed
      // replacement. A current positive query, however, identifies a new family.
      if (!present) return;
      value.connected &= ~family;
      end_if_absent(value);
      auto& addresses = value.scope.addresses;
      auto end = std::remove_if(addresses.begin(), addresses.end(),
          [family](const Address& address) { return address.family == family; });
      address_count_ -= static_cast<size_t>(addresses.end() - end);
      addresses.erase(end, addresses.end());
    }
    old_index = index;
    if (present) value.connected |= family;
    else value.connected &= ~family;
    end_if_absent(value);
  }

  // A complete GetIfEntry2 down/not-found result establishes whole-interface
  // departure. Never read operational state from the partial callback row.
  void interface_absent(uint64_t luid) {
    auto found = scopes_.find(luid);
    if (found == scopes_.end()) return;
    found->second.connected = 0;
    found->second.lifetime = 0;
  }

  void address_changed(uint64_t luid, uint32_t index, const Address& address, bool present) {
    auto found = scopes_.find(luid);
    if (found == scopes_.end()) return;
    if (!valid_family(address.family) || !index) { unavailable_ = true; return; }
    auto& value = found->second;
    uint32_t expected = address.family == kIpv4 ? value.scope.ipv4_index : value.scope.ipv6_index;
    // A family notification supplies a changed index. Until it arrives, an
    // address of another index cannot resurrect or modify this observed family.
    if (index != expected) return;
    auto& addresses = value.scope.addresses;
    auto at = std::find(addresses.begin(), addresses.end(), address);
    if (present && at == addresses.end()) {
      if (addresses.size() == kMaxAddressesPerScope || address_count_ == kMaxAddresses) {
        capacity_ = true;
        return;
      }
      addresses.push_back(address);
      ++address_count_;
    } else if (!present && at != addresses.end()) {
      addresses.erase(at);
      --address_count_;
    }
    // Address events do not resurrect an explicitly deleted IP interface.
    end_if_absent(value);
  }

  void unavailable(uint64_t luid) {
    if (contains(luid)) unavailable_ = true;
  }
  void unavailable() { unavailable_ = true; }

  // A complete, notification-stable snapshot confirms the currently eligible
  // scopes. A mismatch with a known event stream stays Retry: it is not an empty
  // network set, and neither a retry budget nor elapsed time invents continuity.
  Result snapshot(const std::vector<Scope>& current, std::vector<uint64_t>* lifetimes) {
    lifetimes->clear();
    if (unavailable_) return Result::Unavailable;
    if (capacity_ || current.size() > kMaxScopes) return Result::Capacity;
    std::map<uint64_t, bool> present;
    size_t addresses = 0;
    for (const auto& scope : current) {
      if (!scope.luid || !scope.index || !scope.families || (scope.families & ~3u) ||
          ((scope.families & kIpv4) && !scope.ipv4_index) ||
          ((scope.families & kIpv6) && !scope.ipv6_index) ||
          !present.emplace(scope.luid, true).second) return Result::Unavailable;
      if (scope.addresses.size() > kMaxAddressesPerScope ||
          scope.addresses.size() > kMaxAddresses - addresses) return Result::Capacity;
      addresses += scope.addresses.size();
      unsigned observed = 0;
      for (size_t at = 0; at < scope.addresses.size(); ++at) {
        const auto& address = scope.addresses[at];
        if (!valid_family(address.family) ||
            std::find(scope.addresses.begin(), scope.addresses.begin() + at, address)
                != scope.addresses.begin() + at) return Result::Unavailable;
        observed |= address.family;
      }
      if (observed != scope.families) return Result::Unavailable;
    }
    auto next = scopes_;
    for (auto at = next.begin(); at != next.end();) {
      if (!present.contains(at->first)) at = next.erase(at);
      else ++at;
    }
    uint64_t next_id = next_lifetime_;
    bool retry = false;
    for (const auto& scope : current) {
      auto found = next.find(scope.luid);
      bool fresh = found == next.end() || found->second.scope.index != scope.index;
      bool ready = true;
      if (!fresh) {
        auto& old = found->second;
        unsigned continuous = available(old) & scope.families;
        if (old.scope.ipv4_index != scope.ipv4_index) continuous &= ~kIpv4;
        if (old.scope.ipv6_index != scope.ipv6_index) continuous &= ~kIpv6;
        // An actual mapped-family replacement cannot reuse the old lifetime.
        if (!continuous &&
            (((scope.families & kIpv4) && old.scope.ipv4_index != scope.ipv4_index) ||
             ((scope.families & kIpv6) && old.scope.ipv6_index != scope.ipv6_index))) fresh = true;
        else if (available(old) != scope.families || !same_addresses(old, scope)) {
          retry = true;
          ready = false;
        }
      }
      if (fresh) {
        Entry value;
        value.scope = scope;
        value.connected = scope.families;
        found = next.insert_or_assign(scope.luid, std::move(value)).first;
      }
      auto& value = found->second;
      if (ready) {
        // Once event history and the complete current snapshot agree, make
        // that snapshot the next baseline. Do not retain removed addresses or
        // stale family metadata indefinitely.
        value.scope = scope;
        value.connected = scope.families;
      }
      if (ready && !value.lifetime && (available(value) & scope.families)) {
        if (next_id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
          return Result::Exhausted;
        }
        value.lifetime = next_id++;
      }
      lifetimes->push_back(value.lifetime);
    }
    size_t next_addresses = 0;
    for (const auto& [id, value] : next) next_addresses += value.scope.addresses.size();
    if (next_addresses > kMaxAddresses) return Result::Capacity;
    // Retain genuine absence and the observed epoch even if a different scope
    // awaits its remaining notifications. No provisional results escape Retry.
    scopes_ = std::move(next);
    address_count_ = next_addresses;
    next_lifetime_ = next_id;
    if (retry) { lifetimes->clear(); return Result::Retry; }
    return Result::Ready;
  }

 private:
  struct Entry {
    Scope scope;
    unsigned connected = 0;
    uint64_t lifetime = 0;
  };
  static bool valid_family(unsigned value) { return value == kIpv4 || value == kIpv6; }
  static unsigned available(const Entry& value) {
    unsigned families = 0;
    for (const auto& address : value.scope.addresses) families |= address.family;
    return families & value.connected;
  }
  static bool same_addresses(const Entry& value, const Scope& current) {
    size_t eligible = 0;
    for (const auto& address : value.scope.addresses) {
      if (!(address.family & value.connected)) continue;
      ++eligible;
      if (std::find(current.addresses.begin(), current.addresses.end(), address)
          == current.addresses.end()) return false;
    }
    return eligible == current.addresses.size();
  }
  static void end_if_absent(Entry& value) {
    if (!available(value)) value.lifetime = 0;
  }
  std::map<uint64_t, Entry> scopes_;
  size_t address_count_ = 0;
  uint64_t next_lifetime_ = 1;
  bool unavailable_ = false, capacity_ = false;
};

// Called on the owner thread outside every callback-needed lock. Failure keeps
// the exact registration owned for the next close attempt; the other watch is
// still attempted, and only actual successful cancellation releases a handle.
template <typename Handle, typename Cancel, typename Released>
bool retire_subscription_watches(Handle& interface_watch, Handle& address_watch,
    Cancel cancel, Released released) {
  bool complete = true;
  for (Handle* watch : {&interface_watch, &address_watch}) {
    if (*watch == Handle{}) continue;
    if (cancel(*watch)) { *watch = Handle{}; released(); }
    else complete = false;
  }
  return complete;
}

}  // namespace gesher::nsd
