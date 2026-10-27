// CI-only real Windows controls for the production native pool. No ART/JNI,
// Java-monitor, provider or VM-shutdown acceptance follows from this fixture.
#include "../../../compat/accessibility/query_pool.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <memory>
#include <string>

using namespace gesher::accessibility;
namespace {
unsigned failures = 0;
void Check(bool okay, const char* name) {
  std::printf("%s query-pool: %s\n", okay ? "PASS" : "FAIL", name);
  std::fflush(stdout);
  if (!okay) ++failures;
}
[[noreturn]] void Fatal(const char* name) {
  std::fprintf(stderr, "FATAL query-pool: %s\n", name);
  std::fflush(stderr);
  ExitProcess(120);  // Never destroy live pool/request storage after a failed barrier.
}
int64_t Millis(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}
bool Until(const std::function<bool()>& predicate, DWORD milliseconds = 1500) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
  do {
    if (predicate()) return true;
    Sleep(2);
  } while (Clock::now() < deadline);
  return predicate();
}
void Require(bool okay, const char* name) { if (!okay) Fatal(name); }

struct SpawnState {
  unsigned calls = 0, created = 0;
  bool fail_second = false;
  std::array<HANDLE, QueryPool::kWorkers> originals{}, probes{};
  std::array<DWORD, QueryPool::kWorkers> ids{};
};
SpawnState* spawning = nullptr;
HANDLE Spawn(LPTHREAD_START_ROUTINE entry, void* value) {
  SpawnState& state = *spawning;
  ++state.calls;
  if (state.fail_second && state.calls == 2) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
  }
  Require(state.created < state.originals.size(), "unexpected extra worker allocation");
  const unsigned slot = state.created;
  HANDLE thread = CreateThread(nullptr, 0, entry, value, 0, &state.ids[slot]);
  Require(thread != nullptr, "fixture could not create actual Windows worker");
  HANDLE probe = nullptr;
  Require(DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &probe,
                          SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0),
          "fixture could not retain independent worker-exit probe");
  state.originals[slot] = thread;
  state.probes[slot] = probe;
  ++state.created;
  return thread;
}

struct Harness {
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::atomic<bool> block{false};
  std::atomic<unsigned> entered{0}, active{0}, maximum{0}, effects{0}, completed{0};
  std::atomic<bool> worker_identity{true}, nested_rejected{false}, self_stop_rejected{false};
  QueryPool* pool = nullptr;
  SpawnState* spawn = nullptr;
};
Harness* executing = nullptr;
void Execute(Query& query) noexcept {
  Harness& state = *executing;
  const unsigned active = state.active.fetch_add(1) + 1;
  unsigned maximum = state.maximum.load();
  while (maximum < active && !state.maximum.compare_exchange_weak(maximum, active)) {}
  bool actual_worker = false;
  for (unsigned slot = 0; slot < state.spawn->created; ++slot) {
    if (state.spawn->ids[slot] == GetCurrentThreadId()) actual_worker = true;
  }
  if (!actual_worker) state.worker_identity = false;
  state.entered.fetch_add(1, std::memory_order_release);
  if (state.block.load()) {
    Require(WaitForSingleObject(state.release, 15000) == WAIT_OBJECT_0,
            "actual callback release barrier expired");
  }
  if (query.id == 99) {
    auto nested = std::make_shared<Query>();
    nested->window = query.window;
    state.nested_rejected = !state.pool->Run(nested);
    state.self_stop_rejected = !state.pool->Stop();
  }
  if (query.Active()) {
    state.effects.fetch_add(1);
    query.node = query.text;
    query.answer = query.id + query.x + query.y;
    query.okay = true;
  }
  state.active.fetch_sub(1);
  state.completed.fetch_add(1, std::memory_order_release);
}

struct Caller {
  QueryPool* pool = nullptr;
  std::shared_ptr<Query> request;
  HANDLE thread = nullptr, gate = nullptr;
  std::atomic<bool> finished{false};
  bool result = false, stop = false;
  int64_t milliseconds = -1;
  static DWORD WINAPI Run(void* pointer) {
    auto& caller = *static_cast<Caller*>(pointer);
    if (caller.gate) Require(WaitForSingleObject(caller.gate, 5000) == WAIT_OBJECT_0, "caller start barrier");
    const auto start = Clock::now();
    caller.result = caller.stop ? caller.pool->Stop() : caller.pool->Run(caller.request);
    caller.milliseconds = Millis(start);
    caller.finished.store(true, std::memory_order_release);
    return 0;
  }
  void Launch(QueryPool* target, const std::shared_ptr<Query>& query) {
    pool = target;
    request = query;
    thread = CreateThread(nullptr, 0, Run, this, 0, nullptr);
    Require(thread != nullptr, "actual native caller creation");
  }
  void Join() {
    Require(thread && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "actual native caller deadline/join");
    DWORD code = 1;
    Require(GetExitCodeThread(thread, &code) && code == 0 && finished.load(std::memory_order_acquire),
            "actual native caller completion status");
    CloseHandle(thread);
    thread = nullptr;
  }
};

struct Scenario {
  SpawnState spawned;
  Harness harness;
  std::unique_ptr<QueryPool> pool;
  bool finished = false;
  explicit Scenario(bool partial = false) {
    Require(harness.release != nullptr, "actual callback release event creation");
    spawned.fail_second = partial;
    spawning = &spawned;
    executing = &harness;
    harness.spawn = &spawned;
    pool = std::make_unique<QueryPool>(Execute, Spawn);
    harness.pool = pool.get();
  }
  ~Scenario() {
    Require(finished, "scenario destroyed before proven worker drain");
    pool.reset();
    CloseHandle(harness.release);
    executing = nullptr;
    spawning = nullptr;
  }
  void Finish() {
    const auto start = Clock::now();
    Require(pool->Stop(), "final bounded pool drain");
    Check(Millis(start) < 3500 && pool->ActiveCount() == 0 && pool->QueuedCount() == 0,
          "successful Stop is bounded with no active/queued callbacks");
    for (unsigned slot = 0; slot < spawned.created; ++slot) {
      DWORD code = 1, flags = 0;
      Check(WaitForSingleObject(spawned.probes[slot], 0) == WAIT_OBJECT_0 &&
            GetExitCodeThread(spawned.probes[slot], &code) && code == 0,
            "independent real worker handle proves thread exited0");
      SetLastError(ERROR_SUCCESS);
      Check(!GetHandleInformation(spawned.originals[slot], &flags) && GetLastError() == ERROR_INVALID_HANDLE,
            "pool closed its actual worker handle exactly once");
      CloseHandle(spawned.probes[slot]);
      spawned.probes[slot] = nullptr;
    }
    finished = true;
  }
};

std::shared_ptr<Query> Request(const std::shared_ptr<WindowEpoch>& epoch, int64_t id = 1) {
  auto query = std::make_shared<Query>();
  query->window = epoch;
  query->hwnd = 0x1234;
  query->id = id;
  return query;
}
void Start(Scenario& scenario) {
  Require(scenario.pool->Start(), "actual pool startup");
  Check(scenario.spawned.calls == 4 && scenario.spawned.created == 4 && scenario.pool->Start(),
        "startup creates exactly four real workers and is idempotent");
}
void WaitEntered(Scenario& scenario, unsigned count) {
  Require(Until([&] { return scenario.harness.entered.load(std::memory_order_acquire) == count; }),
          "expected real callback admission barrier");
}

void TerminalAndPartialStartup() {
  {
    Scenario scenario;
    Check(scenario.pool->Stop() && !scenario.pool->Start() && scenario.spawned.calls == 0,
          "Stop before Start is terminal and allocates no worker");
    auto query = Request(std::make_shared<WindowEpoch>(1));
    const auto start = Clock::now();
    Check(!scenario.pool->Run(query) && Millis(start) < 500, "terminal pool rejects admission immediately");
    scenario.Finish();
  }
  {
    Scenario scenario(true);
    const auto start = Clock::now();
    Check(!scenario.pool->Start() && Millis(start) < 3500 && scenario.spawned.calls == 2 &&
          scenario.spawned.created == 1 && !scenario.pool->Start(),
          "second-allocation injection joins the actual first worker and remains terminal");
    scenario.Finish();
  }
}

void PositiveAndNested() {
  Scenario scenario;
  Start(scenario);
  auto epoch = std::make_shared<WindowEpoch>(2);
  auto query = Request(epoch, 12);
  query->x = 17; query->y = 19;
  std::wstring external = L"native owned payload \u03a9";
  query->text = external;
  scenario.harness.block = true;
  Caller caller;
  caller.Launch(scenario.pool.get(), query);
  WaitEntered(scenario, 1);
  Check(scenario.pool->Remaining(query->token) > 0, "active worker token exposes the same original budget");
  external.assign(8192, L'X');
  SetEvent(scenario.harness.release);
  caller.Join();
  Check(caller.result && query->answer == 48 && query->node == L"native owned payload \u03a9" &&
        scenario.harness.worker_identity.load(), "real worker returns deep copied payload and exact result");
  Check(!scenario.pool->Run(query), "a completed request is single use");
  auto nested = Request(epoch, 99);
  Check(scenario.pool->Run(nested) && scenario.harness.nested_rejected && scenario.harness.self_stop_rejected,
        "worker rejects nested submission and self Stop without consuming another worker");
  scenario.Finish();
}

void SaturationAndDeadlines() {
  Scenario scenario;
  Start(scenario);
  scenario.harness.block = true;
  auto epoch = std::make_shared<WindowEpoch>(3);
  auto expired_without_submission = Request(epoch);
  std::array<Caller, QueryPool::kWorkers + QueryPool::kQueued> callers;
  for (size_t index = 0; index < 4; ++index) callers[index].Launch(scenario.pool.get(), Request(epoch));
  WaitEntered(scenario, 4);
  for (size_t index = 4; index < callers.size(); ++index) callers[index].Launch(scenario.pool.get(), Request(epoch));
  Require(Until([&] { return scenario.pool->QueuedCount() == 32; }), "full bounded queue admission barrier");
  auto excess = Request(epoch);
  const auto reject = Clock::now();
  Check(!scenario.pool->Run(excess) && Millis(reject) < 500 && scenario.pool->ActiveCount() == 4 &&
        scenario.pool->QueuedCount() == 32, "four active plus32queued is a hard admission limit");
  for (auto& caller : callers) {
    caller.Join();
    Check(!caller.result && caller.milliseconds >= 2400 && caller.milliseconds < 4000,
          "active or queued native caller returns unavailable at original three second deadline");
  }
  Require(Until([&] { return scenario.pool->QueuedCount() == 0; }), "expired queue pruning");
  const auto expired = Clock::now();
  Check(!scenario.pool->Run(expired_without_submission) && Millis(expired) < 500,
        "already expired request cannot acquire a fresh admission budget");
  SetEvent(scenario.harness.release);
  Require(Until([&] { return scenario.pool->ActiveCount() == 0; }), "expired active worker drain");
  Check(scenario.harness.entered == 4 && scenario.harness.completed == 4 && scenario.harness.effects == 0 &&
        scenario.harness.maximum == 4, "expired queued work never executes and late workers have no effect");
  auto fresh = Request(epoch, 6);
  Check(scenario.pool->Run(fresh) && fresh->answer == 6, "same live pool accepts fresh work after expired saturation");
  scenario.Finish();
}

void RetirementAndReuse() {
  Scenario scenario;
  Start(scenario);
  scenario.harness.block = true;
  auto old = std::make_shared<WindowEpoch>(4);
  std::array<Caller, 6> callers;
  for (size_t index = 0; index < 4; ++index) callers[index].Launch(scenario.pool.get(), Request(old));
  WaitEntered(scenario, 4);
  for (size_t index = 4; index < callers.size(); ++index) callers[index].Launch(scenario.pool.get(), Request(old));
  Require(Until([&] { return scenario.pool->QueuedCount() == 2; }), "retirement queued barrier");
  scenario.pool->Retire(old);
  Check(!old->alive && scenario.pool->QueuedCount() == 0 && scenario.pool->Remaining(callers[0].request->token) == 0,
        "retirement cancels active tokens and removes only stale queued generation");
  auto replacement = Request(std::make_shared<WindowEpoch>(5), 7);  // Same numeric HWND, new shared lifetime.
  Caller next;
  next.Launch(scenario.pool.get(), replacement);
  Require(Until([&] { return scenario.pool->QueuedCount() == 1; }), "new HWND generation admission");
  SetEvent(scenario.harness.release);
  next.Join();
  for (auto& caller : callers) { caller.Join(); Check(!caller.result, "retired generation returns unavailable"); }
  Check(next.result && replacement->answer == 7 && scenario.harness.entered == 5 && scenario.harness.effects == 1,
        "reused numeric HWND preserves old cancellation while fresh generation succeeds");
  scenario.Finish();
}

void ConcurrentStopAndWaitingCaller() {
  Scenario scenario;
  Start(scenario);
  scenario.harness.block = true;
  auto epoch = std::make_shared<WindowEpoch>(6);
  std::array<Caller, 4> active;
  for (auto& caller : active) caller.Launch(scenario.pool.get(), Request(epoch));
  WaitEntered(scenario, 4);
  Caller queued;
  queued.Launch(scenario.pool.get(), Request(epoch));
  Require(Until([&] { return scenario.pool->QueuedCount() == 1; }), "shutdown queued native waiter barrier");
  HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(gate != nullptr, "concurrent Stop event");
  std::array<Caller, 2> stops;
  for (auto& caller : stops) { caller.stop = true; caller.gate = gate; caller.Launch(scenario.pool.get(), nullptr); }
  SetEvent(gate);
  Require(Until([&] { return active[0].request->canceled.load() && scenario.pool->QueuedCount() == 0; }),
          "actual Stop cancellation publication");
  SetEvent(scenario.harness.release);
  for (auto& caller : stops) {
    caller.Join();
    Check(caller.result && caller.milliseconds < 3500, "both concurrent Stop callers serialize and complete bounded");
  }
  CloseHandle(gate);
  bool queued_done = false;
  { std::lock_guard<std::mutex> lock(queued.request->result_mutex); queued_done = queued.request->done; }
  Check(scenario.pool->ActiveCount() == 0 && !queued.finished.load() && !queued_done,
        "successful callback drain does not falsely imply a removed native Run waiter returned");
  for (auto& caller : active) { caller.Join(); Check(!caller.result, "Stop cancels active caller result"); }
  queued.Join();
  Check(!queued.result && queued.milliseconds < 4000, "removed native waiter returns by its original deadline");
  Check(!scenario.pool->Start() && !scenario.pool->Run(Request(epoch)), "Stop remains terminal after worker drain");
  scenario.Finish();
}

void FailedStopThenDrain() {
  Scenario scenario;
  Start(scenario);
  scenario.harness.block = true;
  Caller caller;
  caller.Launch(scenario.pool.get(), Request(std::make_shared<WindowEpoch>(7)));
  WaitEntered(scenario, 1);
  const auto start = Clock::now();
  Check(!scenario.pool->Stop() && Millis(start) >= 2400 && Millis(start) < 4000 &&
        scenario.pool->ActiveCount() == 1, "stuck actual callback makes Stop fail bounded without claiming drain");
  Check(!scenario.pool->Start(), "failed Stop never reopens admission");
  caller.Join();
  Check(!caller.result && scenario.harness.effects == 0, "deadline caller may finish while actual callback remains alive");
  SetEvent(scenario.harness.release);
  scenario.Finish();
  Check(scenario.harness.completed == 1 && scenario.harness.effects == 0,
        "retained failed-Stop state safely drains after external barrier release");
}

void ResourceChecked(void (*test)(), const char* label) {
  DWORD before = 0, after = 0;
  Require(GetProcessHandleCount(GetCurrentProcess(), &before), "baseline process handle count");
  test();
  Require(GetProcessHandleCount(GetCurrentProcess(), &after), "final process handle count");
  Check(before == after, label);
  if (before != after) std::printf("HANDLE_COUNT before=%lu after=%lu\n", before, after);
}
}  // namespace

int wmain() {
  wchar_t ci[16];
  const DWORD length = GetEnvironmentVariableW(L"GITHUB_ACTIONS", ci, 16);
  if (!length || length >= 16 || std::wcscmp(ci, L"true") != 0) {
    std::fputs("Native pool fixture executes only in CI\n", stderr);
    return 90;
  }
  ResourceChecked(TerminalAndPartialStartup, "terminal/partial-start scenarios return every Windows handle");
  ResourceChecked(PositiveAndNested, "positive/nested scenarios return every Windows handle");
  ResourceChecked(SaturationAndDeadlines, "saturation/deadline scenarios return every Windows handle");
  ResourceChecked(RetirementAndReuse, "retirement/reuse scenario returns every Windows handle");
  ResourceChecked(ConcurrentStopAndWaitingCaller, "concurrent Stop/waiter scenario returns every Windows handle");
  ResourceChecked(FailedStopThenDrain, "failed Stop/retry scenario returns every Windows handle");
  std::printf("ACTUAL_QUERY_POOL_SUMMARY failures=%u; workers=4 queue=32 budget_ms=3000; scope=native-core-only\n", failures);
  return failures == 0 ? 0 : 1;
}
