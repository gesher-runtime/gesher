// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Finite ordinary lifetime observations. No sockets or host interfaces change.
#include <gesher/nsd_subscription_lifetimes.h>

#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

using Lifetimes = gesher::nsd::SubscriptionLifetimes;
using Result = Lifetimes::Result;
static unsigned checks;
static void check(bool success, const char* message) {
  ++checks;
  if (!success) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
static Lifetimes::Address address(unsigned family, unsigned value = 1) {
  Lifetimes::Address out;
  out.family = family;
  out.bytes[0] = family == 1 ? 192 : 0xfe;
  out.bytes[1] = family == 1 ? 0 : 0x80;
  out.bytes[2] = static_cast<unsigned char>(value >> 8);
  out.bytes[family == 1 ? 3 : 15] = static_cast<unsigned char>(value);
  return out;
}
static Lifetimes::Scope scope(uint64_t luid = 101, unsigned families = 3) {
  Lifetimes::Scope out{luid, 11, 11, 17, families, {}};
  if (families & 1) out.addresses.push_back(address(1));
  if (families & 2) out.addresses.push_back(address(2));
  return out;
}
static uint64_t observe(Lifetimes& state, const Lifetimes::Scope& current) {
  std::vector<uint64_t> ids;
  check(state.snapshot({current}, &ids) == Result::Ready, "complete ordinary scope ready");
  check(ids.size() == 1 && ids[0] > 0, "one positive observed lifetime");
  return ids[0];
}
static void unchanged_and_related_notifications() {
  Lifetimes state;
  auto dual = scope();
  uint64_t initial = observe(state, dual);
  check(observe(state, dual) == initial, "unchanged snapshot retains lifetime");
  state.interface_changed(999, 1, 29, false);
  state.interface_absent(999);
  state.address_changed(999, 29, address(1), false);
  state.unavailable(999);
  check(observe(state, dual) == initial, "unrelated LUID cannot retire selected scope");
  state.interface_changed(101, 1, 11, true);
  state.interface_changed(101, 2, 17, true);
  state.address_changed(101, 11, address(1), true);
  check(observe(state, dual) == initial, "metric and unchanged parameter observations preserve lifetime");
  check(state.size() == 1 && state.address_count() == 2, "unchanged events do not accumulate state");
}
static void independent_family_continuity() {
  Lifetimes state;
  uint64_t initial = observe(state, scope());
  state.interface_changed(101, 1, 11, false);
  check(observe(state, scope(101, 2)) == initial, "IPv6 bridges IPv4 removal");
  check(state.address_count() == 1, "accepted complete snapshot releases removed-family address");
  state.interface_changed(101, 1, 11, true);
  state.address_changed(101, 11, address(1), true);
  check(observe(state, scope()) == initial, "IPv4 return keeps genuinely continuous IPv6 scope");

  auto renamed = scope();
  renamed.ipv6_index = 19;
  state.interface_changed(101, 2, 19, true);
  state.address_changed(101, 19, address(2), true);
  check(observe(state, renamed) == initial, "IPv4 bridges actual IPv6 index replacement");
  state.interface_changed(101, 2, 17, false);
  state.address_changed(101, 17, address(2), false);
  check(observe(state, renamed) == initial, "delayed old-index deletion cannot delete replacement family");

  Lifetimes handover;
  initial = observe(handover, scope(101, 1));
  handover.interface_changed(101, 2, 17, true);
  handover.address_changed(101, 17, address(2), true);
  handover.interface_changed(101, 1, 11, false);
  check(observe(handover, scope(101, 2)) == initial, "overlapping family handover preserves observed continuity");
}
static void between_poll_departure_and_return() {
  Lifetimes state;
  uint64_t initial = observe(state, scope());
  state.interface_changed(101, 1, 11, false);
  state.interface_changed(101, 2, 17, false);
  state.interface_changed(101, 1, 11, true);
  state.interface_changed(101, 2, 17, true);
  uint64_t returned = observe(state, scope());
  check(returned != initial, "whole-family departure and return between polls starts a new lifetime");
  state.interface_absent(101);
  state.interface_changed(101, 1, 11, true);
  state.interface_changed(101, 2, 17, true);
  check(observe(state, scope()) != returned, "complete operational down/up observation starts new lifetime");

  Lifetimes single;
  initial = observe(single, scope(101, 1));
  single.address_changed(101, 11, address(1), false);
  std::vector<uint64_t> ids;
  for (unsigned i = 0; i < 3; ++i) {
    check(single.snapshot({scope(101, 1)}, &ids) == Result::Retry && ids.empty(),
        "unreconciled callback state stays retry without invented absence");
  }
  single.address_changed(101, 11, address(1), true);
  check(observe(single, scope(101, 1)) != initial, "last eligible address departure and return starts new lifetime");
}
static void address_reconciliation() {
  Lifetimes state;
  auto current = scope(101, 1);
  uint64_t initial = observe(state, current);
  auto changed = current;
  changed.addresses[0] = address(1, 2);
  std::vector<uint64_t> ids;
  check(state.snapshot({changed}, &ids) == Result::Retry && ids.empty(),
      "a changed address snapshot cannot invent the unobserved event ordering");
  state.address_changed(101, 11, address(1, 2), true);
  state.address_changed(101, 11, address(1), false);
  check(observe(state, changed) == initial, "add-before-remove address handover stays continuous");
  check(state.address_count() == 1, "reconciled address state is bounded to complete snapshot");
  state.address_changed(101, 11, address(1, 2), false);
  state.address_changed(101, 11, address(1, 3), true);
  changed.addresses[0] = address(1, 3);
  check(observe(state, changed) != initial, "remove-before-add address handover records the genuine gap");
}
static void identity_and_observation_limits() {
  Lifetimes state;
  auto current = scope();
  uint64_t initial = observe(state, current);
  current.index = current.ipv4_index = 31;
  check(observe(state, current) != initial, "different mapped interface identity cannot reuse cached service");
  initial = observe(state, current);
  current.luid = 202;
  check(observe(state, current) != initial, "replacement LUID cannot reuse lifetime");
  std::vector<uint64_t> ids;
  initial = observe(state, current);
  check(state.snapshot({}, &ids) == Result::Ready && ids.empty(), "complete empty snapshot establishes absence");
  check(state.size() == 0 && state.address_count() == 0, "absent scopes release bounded state");
  check(observe(state, current) != initial, "same LUID after observed absence receives fresh lifetime");
  state.unavailable(current.luid);
  check(state.snapshot({current}, &ids) == Result::Unavailable && ids.empty(),
      "unknown complete-state observation is explicitly unavailable, not fabricated continuity");
}
static void ordinary_capacity_limits() {
  std::vector<uint64_t> ids;
  Lifetimes many;
  std::vector<Lifetimes::Scope> scopes;
  for (size_t i = 0; i < Lifetimes::kMaxScopes; ++i) {
    auto current = scope(i + 1, 1);
    current.index = current.ipv4_index = static_cast<uint32_t>(i + 11);
    scopes.push_back(std::move(current));
  }
  check(many.snapshot(scopes, &ids) == Result::Ready && ids.size() == scopes.size(),
      "scope budget supports its complete documented capacity");
  scopes.push_back(scope(scopes.size() + 1, 1));
  check(many.snapshot(scopes, &ids) == Result::Capacity && ids.empty(),
      "too many actual scopes report resource unavailability without truncation");
  Lifetimes addresses;
  auto current = scope(101, 1);
  current.addresses.clear();
  for (size_t i = 0; i < Lifetimes::kMaxAddressesPerScope; ++i) current.addresses.push_back(address(1, i + 1));
  observe(addresses, current);
  addresses.address_changed(101, 11, address(1, 65), true);
  check(addresses.snapshot({current}, &ids) == Result::Capacity && ids.empty(),
      "ordinary additional address beyond capacity reports unavailability");
  check(addresses.address_count() == Lifetimes::kMaxAddressesPerScope,
      "capacity rejection does not retain an unbounded notification entry");
}
static void callback_snapshot_fence() {
  gesher::nsd::SubscriptionSnapshotFence fence;
  std::atomic<uint64_t> generation{1};
  check(fence.stable(generation, 1), "initial snapshot baseline has no fabricated departure");
  std::promise<void> started, release;
  auto released = release.get_future();
  std::thread callback([&] {
    fence.begin(generation);
    started.set_value();
    released.wait();
    fence.end(generation);
  });
  started.get_future().wait();
  check(!fence.stable(generation, generation.load()), "in-flight callback blocks snapshot even at current generation");
  release.set_value();
  callback.join();
  check(!fence.stable(generation, 1), "callback overlapping snapshot invalidates prior capture");
  check(fence.stable(generation, generation.load()), "completed observation permits new stable capture");
  // A callback that represents an initial/null notification still invalidates
  // packet capture, but does not itself retire a tracked interface.
  Lifetimes state;
  uint64_t initial = observe(state, scope());
  fence.begin(generation); fence.end(generation);
  check(observe(state, scope()) == initial, "initial/null callback is not departure evidence");
}
static void owned_watch_retirement() {
  int interface_watch = 10, address_watch = 20;
  unsigned attempts = 0, released = 0;
  auto release = [&] { ++released; };
  bool first = gesher::nsd::retire_subscription_watches(interface_watch, address_watch,
      [&](int watch) { ++attempts; return watch == 20; }, release);
  check(!first && interface_watch == 10 && address_watch == 0,
      "failed cancellation retains exact owner while successful watch is released");
  check(attempts == 2 && released == 1, "one cancellation failure cannot skip the other watch");
  bool second = gesher::nsd::retire_subscription_watches(interface_watch, address_watch,
      [&](int watch) { ++attempts; return watch == 10; }, release);
  check(second && !interface_watch && !address_watch, "later actual success retires remaining owner");
  check(attempts == 3 && released == 2, "released handles are not canceled twice");
  check(gesher::nsd::retire_subscription_watches(interface_watch, address_watch,
      [&](int) { ++attempts; return true; }, release), "retirement is idempotent");
  check(attempts == 3 && released == 2, "already retired owner releases no additional resources");
}
int main() {
  unchanged_and_related_notifications();
  independent_family_continuity();
  between_poll_departure_and_return();
  address_reconciliation();
  identity_and_observation_limits();
  ordinary_capacity_limits();
  callback_snapshot_fence();
  owned_watch_retirement();
  std::cout << "NSD_SUBSCRIPTION_LIFETIMES_OK checks=" << checks << '\n';
}
