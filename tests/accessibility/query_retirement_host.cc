// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "query_pool.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
using namespace gesher::accessibility;
namespace {
std::mutex gate_mutex;
std::condition_variable gate_changed;
bool release_callback = false;
std::atomic<unsigned> entered{0}, completed{0}, effects{0};
unsigned failures = 0, checks = 0;
void Check(bool value, const char* text) {
  ++checks;
  std::printf("%s retire-repro: %s\n", value ? "PASS" : "FAIL", text);
  if (!value) ++failures;
}
void Require(bool value, const char* text) {
  if (!value) { std::fprintf(stderr, "FATAL %s\n", text); std::fflush(stderr); std::_Exit(120); }
}
bool Until(const std::function<bool()>& condition, int milliseconds) {
  auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
  while (!condition() && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return condition();
}
void Execute(Query& query) noexcept {
  entered.fetch_add(1, std::memory_order_release);
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    gate_changed.wait(lock, [] { return release_callback; });
  }
  if (query.Active()) { ++effects; query.okay = true; }
  completed.fetch_add(1, std::memory_order_release);
}
struct Caller {
  std::shared_ptr<Query> request;
  std::thread thread;
  std::atomic<bool> returned{false};
  bool result = true;
  Clock::time_point finished;
  void Start(QueryPool& pool, const std::shared_ptr<WindowEpoch>& epoch) {
    request = std::make_shared<Query>(); request->window = epoch; request->hwnd = 0x1234;
    thread = std::thread([this, &pool] {
      result = pool.Run(request); finished = Clock::now(); returned.store(true, std::memory_order_release);
    });
  }
};

void ReleaseCallbacks(bool release) {
  { std::lock_guard<std::mutex> lock(gate_mutex); release_callback = release; }
  if (release) gate_changed.notify_all();
}

void PrepublishedPruneAndDeadline() {
  QueryPool pool(Execute);
  Require(pool.Start(), "start prepublished retirement pool");
  ReleaseCallbacks(false);
  const unsigned previous = entered.load(), completed_before = completed.load(), effects_before = effects.load();
  auto epoch = std::make_shared<WindowEpoch>(92);
  std::array<Caller, 5> callers;
  std::array<std::weak_ptr<Query>, 5> lifetime;
  for (size_t i = 0; i < 4; ++i) { callers[i].Start(pool, epoch); lifetime[i] = callers[i].request; }
  Require(Until([&] { return entered.load() == previous + 4; }, 1500), "prepublication callbacks active");
  callers[4].Start(pool, epoch); lifetime[4] = callers[4].request;
  Require(Until([&] { return pool.QueuedCount() == 1; }, 1500), "prepublication request queued");
  // The actual bridge publishes this before calling QueryPool::Retire. An
  // unrelated retirement must notify any dead queued request that it prunes.
  epoch->alive.store(false, std::memory_order_release);
  const auto retired_at = Clock::now();
  pool.Retire(std::make_shared<WindowEpoch>(93));
  Check(pool.QueuedCount() == 0 && Until([&] {
          return callers[4].returned.load(std::memory_order_acquire);
        }, 500), "pruning a prepublished dead epoch wakes its queued caller before matching Retire");
  pool.Retire(epoch);
  for (auto& caller : callers) {
    Require(Until([&] { return caller.returned.load(std::memory_order_acquire); }, 4000),
            "prepublication caller deadline");
    caller.thread.join();
    Check(!caller.result, "prepublished retired request returns unavailable");
    caller.request.reset();
  }
  const auto queued_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      callers[4].finished - retired_at).count();
  Check(pool.ActiveCount() == 4 && completed == completed_before,
        "prepublication caller return retains all executing callback owners");
  for (size_t i = 0; i < 4; ++i) Check(!lifetime[i].expired(), "prepublication retains active request lifetime");
  Check(lifetime[4].expired(), "pruned queued request releases after its caller returns");

  // A fresh queued caller must be able to prune itself at its original deadline
  // while all four canceled callbacks remain blocked. This catches recursive
  // result-mutex acquisition and pool/result lock-order regressions.
  Caller expired;
  expired.Start(pool, std::make_shared<WindowEpoch>(94));
  Require(Until([&] { return pool.QueuedCount() == 1; }, 1500), "queued deadline admission");
  Require(Until([&] { return expired.returned.load(std::memory_order_acquire); }, 4000),
          "queued deadline returns without a pruning self-deadlock");
  expired.thread.join();
  Check(!expired.result && expired.request->canceled && !expired.request->done && pool.QueuedCount() == 0,
        "queued deadline cancels and prunes without declaring execution complete");
  Check(pool.ActiveCount() == 4 && completed == completed_before,
        "queued deadline cannot free the still blocked callbacks");
  expired.request.reset();
  ReleaseCallbacks(true);
  Require(Until([&] { return pool.ActiveCount() == 0; }, 1500), "prepublication active drain");
  Require(pool.Stop(), "prepublication workers join");
  Check(completed == completed_before + 4 && effects == effects_before,
        "prepublished canceled callbacks retire without late effects");
  for (const auto& item : lifetime) Check(item.expired(), "prepublication request lifetime fully drained");
  Check(qa_handles == 0, "prepublication scenario returns all worker handles");
  std::printf("QUERY_PRUNE_REPRO queued_retirement_return_ms=%lld; original queued deadline preserved\n",
              static_cast<long long>(queued_ms));
}

void RetirementRaces() {
  QueryPool pool(Execute);
  Require(pool.Start(), "start race pool");
  for (unsigned round = 0; round < 128; ++round) {
    ReleaseCallbacks(false);
    const unsigned previous = entered.load();
    const unsigned effects_before = effects.load();
    auto epoch = std::make_shared<WindowEpoch>(1000 + round * 2);
    Caller caller;
    caller.Start(pool, epoch);
    if ((round & 1) == 0) {
      Require(Until([&] { return entered.load() == previous + 1; }, 1500), "race callback entry");
    } else {
      // submitted is published before queue insertion/result-wait registration.
      // Retire may win admission or race the caller entering its result wait.
      Require(Until([&] { return caller.request->submitted.load(); }, 1500), "race submission entry");
    }
    pool.Retire(epoch);
    const bool prompt = Until([&] { return caller.returned.load(std::memory_order_acquire); }, 500);
    Check(prompt, "retirement racing with admission or wait registration loses no wakeup");
    Require(Until([&] { return caller.returned.load(std::memory_order_acquire); }, 4000), "race caller deadline");
    caller.thread.join();
    Check(!caller.result, "retirement race returns unavailable");
    const bool executing = entered.load() != previous;
    if (executing) {
      bool done;
      { std::lock_guard<std::mutex> lock(caller.request->result_mutex); done = caller.request->done; }
      Check(!done && pool.ActiveCount() == 1,
            "prompt caller return does not mark the blocked callback done");
    }
    std::weak_ptr<Query> lifetime = caller.request;
    caller.request.reset();
    if (executing) Check(!lifetime.expired(), "racing cancellation retains the executing request");
    ReleaseCallbacks(true);
    Require(Until([&] { return pool.ActiveCount() == 0; }, 1500), "race active owner drain");
    Check(effects == effects_before, "canceled callback returns without a late effect");
    // Worker clears active_ before its loop-local shared_ptr is destroyed.
    // Observe that actual lifetime release separately from the active counter.
    Check(Until([&] { return lifetime.expired(); }, 500),
          "the worker releases its final request owner after the callback returns");
    auto fresh = std::make_shared<Query>();
    fresh->window = std::make_shared<WindowEpoch>(1001 + round * 2);
    fresh->hwnd = 0x1234;  // Same numeric HWND, distinct lifetime.
    Check(pool.Run(fresh) && fresh->done && fresh->okay,
          "fresh HWND generation still returns an actual successful result");
    Require(Until([&] { return pool.ActiveCount() == 0; }, 1500), "fresh result owner drain");
  }
  for (unsigned round = 0; round < 128; ++round) {
    const unsigned previous = entered.load();
    auto epoch = std::make_shared<WindowEpoch>(2000 + round);
    Caller caller;
    caller.Start(pool, epoch);
    if ((round & 1) == 0) {
      Require(Until([&] { return caller.request->submitted.load(); }, 1500), "completion race submission");
    } else {
      Require(Until([&] { return entered.load() == previous + 1; }, 1500), "completion race worker entry");
    }
    pool.Retire(epoch);
    Require(Until([&] { return caller.returned.load(std::memory_order_acquire); }, 500), "completion/cancel caller return");
    caller.thread.join();
    {
      std::lock_guard<std::mutex> lock(caller.request->result_mutex);
      Check(!caller.result || caller.request->done,
            "success racing cancellation is published only after actual completion");
    }
    Require(Until([&] { return pool.ActiveCount() == 0; }, 1500), "completion race active drain");
    std::weak_ptr<Query> lifetime = caller.request;
    caller.request.reset();
    Check(Until([&] { return lifetime.expired(); }, 500),
          "completion/cancel race releases the request exactly once");
  }
  Require(pool.Stop(), "race pool workers join");
  Check(qa_handles == 0, "race pool returns all worker handles");
  std::puts("QUERY_RETIRE_RACES registration=128 completion=128 fresh-generation=128; running owners retained until callback return");
}
}
int main() {
  QueryPool pool(Execute);
  Require(pool.Start(), "start production pool");
  auto epoch = std::make_shared<WindowEpoch>(91);
  std::array<Caller, 5> callers;
  std::array<std::weak_ptr<Query>, 5> lifetime;
  for (size_t i = 0; i < 4; ++i) { callers[i].Start(pool, epoch); lifetime[i] = callers[i].request; }
  Require(Until([] { return entered.load(std::memory_order_acquire) == 4; }, 1500), "four callbacks active");
  callers[4].Start(pool, epoch); lifetime[4] = callers[4].request;
  Require(Until([&] { return pool.QueuedCount() == 1; }, 1500), "fifth request is genuinely queued");
  const auto retired_at = Clock::now();
  pool.Retire(epoch);
  Check(!epoch->alive && pool.QueuedCount() == 0, "retirement removes old queued generation");
  bool prompt = Until([&] {
    for (auto& caller : callers) if (!caller.returned.load(std::memory_order_acquire)) return false;
    return true;
  }, 500);
  Check(prompt, "retired active and queued callers wake without waiting for original three-second deadline");
  Require(Until([&] {
    for (auto& caller : callers) if (!caller.returned.load(std::memory_order_acquire)) return false;
    return true;
  }, 4000), "all callers return by original deadline");
  long maximum_ms = 0;
  for (auto& caller : callers) {
    caller.thread.join();
    maximum_ms = std::max(maximum_ms, static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        caller.finished - retired_at).count()));
    Check(!caller.result, "retired generation returns unavailable");
    caller.request.reset();
  }
  Check(pool.ActiveCount() == 4 && completed == 0, "caller return does not release four executing callbacks");
  for (size_t i = 0; i < 4; ++i) Check(!lifetime[i].expired(), "pool retains executing request lifetime");
  Check(lifetime[4].expired(), "removed inactive request can retire after its caller returns");
  { std::lock_guard<std::mutex> lock(gate_mutex); release_callback = true; }
  gate_changed.notify_all();
  Require(Until([&] { return pool.ActiveCount() == 0; }, 1500), "actual callback return drains active ownership");
  Require(pool.Stop(), "production stop joins all returned workers");
  Check(completed == 4 && effects == 0, "canceled callbacks return without late effects");
  for (const auto& item : lifetime) Check(item.expired(), "all request lifetimes retired after actual callbacks return");
  Check(qa_handles == 0, "all host worker handles retired");
  // The historical negative control already fails the direct retirement case;
  // avoid repeating its full three-second delay for every race iteration.
  if (failures == 0) PrepublishedPruneAndDeadline();
  if (failures == 0) RetirementRaces();
  std::printf("QUERY_RETIRE_REPRO retirement_to_last_return_ms=%ld checks=%u failures=%u scope=exact-production-owner-host-thread-boundary\n",
              maximum_ms, checks, failures);
  return failures == 0 ? 0 : 1;
}
