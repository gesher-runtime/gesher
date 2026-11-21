// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Test-only qualification of the pinned LLVM native cleanup primitive.
// This is not a translator implementation or portable C++ longjmp contract.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unwind.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <initializer_list>
#include "cleanup_probe.h"
#include "../../../compat/arm64/dll/host_call.h"

struct alignas(16) Checkpoint {
  uint64_t gpr[8], rsp, rip, xmm[20];
};
static_assert(offsetof(Checkpoint, rsp) == 64);
static_assert(offsetof(Checkpoint, xmm) == 80 && sizeof(Checkpoint) == 240);
extern "C" __attribute__((returns_twice)) int CleanupSave(Checkpoint*);
extern "C" [[noreturn]] void CleanupRestore(Checkpoint*);

struct EntryFrame {
  uint64_t gpr[8], xmm[8];
  const uint64_t* stack;
  uint64_t out_rdx, out_xmm0;
};
extern "C" uint64_t gesher_call_host_sysv(HostCall*);
extern "C" __attribute__((sysv_abi)) uint64_t CleanupGuestEntry(
    CleanupLink*, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t);

enum Kind { Normal, EscapeOwner, NestedOuter, NearestInner };
enum Mode { Supported, CatchAll, Noexcept, ForeignFinally, RawRejected, ForeignNormal };
struct Owner;
struct Token { Owner* slot; uint64_t generation; DWORD thread; };
struct Owner {
  Checkpoint checkpoint;
  _Unwind_Exception exception;
  CleanupLink link;
  Owner* parent;
  Token token;
  uintptr_t cfa, region;
  unsigned expected_resources, normal, landed, released, stop_seen;
  bool active, requested;
};
struct State {
  Owner owners[4];
  Owner* top;
  Token previous[2];
  unsigned depth, resource_depth, created, destroyed, checks, failures, cases;
  unsigned resource_stack[64], after[2][100], destroyed_owners[2];
  unsigned foreign_open, foreign_close, catch_count, stop_calls, transfers;
  Mode mode;
  Kind kind;
  bool worker;
};
static thread_local State* state;
static volatile LONG64 next_generation;
static uint64_t (*dll_call)(CleanupLink*);
static constexpr uint64_t kClass = UINT64_C(0x474553484a4d5000);
static constexpr uint64_t kResult = 77;

[[noreturn]] static void Fatal(const char* message, unsigned code = 70) {
  std::printf("%s\n", message);
  ExitProcess(code);
}
static void Check(bool ok, const char* message) {
  ++state->checks;
  if (!ok) {
    ++state->failures;
    std::printf("FAIL native cleanup: %s (kind=%d mode=%d thread=%lu)\n",
                message, state->kind, state->mode, GetCurrentThreadId());
  }
}
static void Require(bool ok, const char* message) {
  Check(ok, message);
  if (!ok) Fatal("FATAL native cleanup invariant");
}

static bool Live(Token token) {
  // Reject a foreign thread before dereferencing any destination. An old token
  // is compared with the current live chain and generation, never resumed.
  if (token.thread != GetCurrentThreadId()) return false;
  for (Owner* owner = state->top; owner != nullptr; owner = owner->parent)
    if (owner == token.slot)
      return owner->active && owner->token.generation == token.generation &&
             owner->token.thread == token.thread;
  return false;
}

static void Notify(void* opaque, int event, int id, int ok) {
  Require(opaque == state, "callback remains on the owning native thread");
  Require(ok != 0, "real resource API succeeded");
  if (event == CLEANUP_ENTER) {
    Require(state->resource_depth < 64, "bounded resource stack");
    state->resource_stack[state->resource_depth++] = static_cast<unsigned>(id);
    ++state->created;
  } else if (event == CLEANUP_LEAVE) {
    Require(state->resource_depth != 0 &&
                state->resource_stack[state->resource_depth - 1] == static_cast<unsigned>(id),
            "cleanup is exactly once in reverse construction order");
    --state->resource_depth;
    ++state->destroyed;
  } else if (event == CLEANUP_AFTER) {
    Require(id / 100 < 2 && id % 100 < 100, "continuation observation bounds");
    ++state->after[id / 100][id % 100];
  } else if (event == CLEANUP_FOREIGN_OPEN) {
    ++state->foreign_open;
  } else if (event == CLEANUP_FOREIGN_CLOSE) {
    ++state->foreign_close;
  } else {
    Fatal("FATAL unknown resource observation");
  }
}

class Resource {
 public:
  Resource(CleanupLink* link, unsigned id) : link_(link), id_(id) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    memory_ = HeapAlloc(GetProcessHeap(), 0, 64);
    Notify(state, CLEANUP_ENTER, id_, event_ != nullptr && memory_ != nullptr);
  }
  ~Resource() noexcept {
    const bool closed = CloseHandle(event_) != 0;
    const bool freed = HeapFree(GetProcessHeap(), 0, memory_) != 0;
    link_->notify(link_->state, CLEANUP_LEAVE, id_, closed && freed);
  }
  Resource(const Resource&) = delete;
  Resource& operator=(const Resource&) = delete;
 private:
  CleanupLink* link_;
  unsigned id_;
  HANDLE event_;
  void* memory_;
};

static uint64_t NativeLoop(CleanupLink*);
__declspec(noinline) static void RunOwner(unsigned level);

class Registration {
 public:
  explicit Registration(Owner* owner) : owner_(owner) {}
  ~Registration() noexcept {
    Require(state->top == owner_ && owner_->active, "nested owner retirement order");
    const unsigned level = owner_->link.level;
    state->previous[level] = owner_->token;
    owner_->active = false;
    state->top = owner_->parent;
    --state->depth;
    ++state->destroyed_owners[level];
  }
 private:
  Owner* owner_;
};

static _Unwind_Reason_Code TraceOwner(_Unwind_Context* context, void* opaque) {
  auto* owner = static_cast<Owner*>(opaque);
  DWORD64 image = 0;
  const auto ip = _Unwind_GetIP(context);
  auto* function = RtlLookupFunctionEntry(ip, &image, nullptr);
  if (function != nullptr &&
      image + function->BeginAddress == reinterpret_cast<uintptr_t>(&RunOwner)) {
    owner->cfa = _Unwind_GetCFA(context);
    owner->region = image + function->BeginAddress;
    return _URC_NORMAL_STOP;
  }
  return _URC_NO_REASON;
}

static void Release(_Unwind_Reason_Code, _Unwind_Exception* exception) {
  auto* owner = reinterpret_cast<Owner*>(
      reinterpret_cast<uint8_t*>(exception) - offsetof(Owner, exception));
  Require(Live(owner->token), "exception storage belongs to a live owner");
  ++owner->released;
}

static _Unwind_Reason_Code Stop(int version, _Unwind_Action actions,
                               uint64_t klass, _Unwind_Exception* exception,
                               _Unwind_Context* context, void* opaque) {
  auto* owner = static_cast<Owner*>(opaque);
  ++state->stop_calls;
  Require(version == 1 && klass == kClass && exception == &owner->exception,
          "pinned forced-unwind stop identity");
  Require((actions & (_UA_FORCE_UNWIND | _UA_CLEANUP_PHASE)) ==
              (_UA_FORCE_UNWIND | _UA_CLEANUP_PHASE),
          "forced cleanup flags");
  Require((actions & _UA_END_OF_STACK) == 0, "destination reached before stack end");
  if (_Unwind_GetCFA(context) != owner->cfa) return _URC_NO_REASON;
  Require(_Unwind_GetRegionStart(context) == owner->region && Live(owner->token),
          "exact live owner CFA and function");
  Require(state->top == owner, "all abandoned owner registrations retired");
  Require(state->resource_depth == owner->expected_resources,
          "all abandoned LLVM resources cleaned before landing");
  if (state->foreign_open != state->foreign_close) {
    if (state->mode == ForeignFinally)
      Fatal("UNSUPPORTED foreign SEH finally was not executed", 73);
    Fatal("FATAL unqualified foreign resource at destination");
  }
  ++owner->stop_seen;
  _Unwind_DeleteException(exception);
  Require(owner->released == 1, "forced exception released exactly once");
  CleanupRestore(&owner->checkpoint);
}

[[noreturn]] static void Transfer(Token token) {
  Require(Live(token), "destination validated before any transfer");
  Owner* const owner = token.slot;
  Require(!owner->requested, "one transfer per owner lifetime");
  owner->requested = true;
  ++state->transfers;
  if (state->mode == RawRejected) {
    // Deliberately rejected native experiment, not a portable C++ baseline:
    // this bypasses nontrivial destructors and is run only in its own process.
    CleanupRestore(&owner->checkpoint);
  }
  owner->exception = {};
  owner->exception.exception_class = kClass;
  owner->exception.exception_cleanup = Release;
  const auto result = _Unwind_ForcedUnwind(&owner->exception, Stop, owner);
  std::printf("FAIL _Unwind_ForcedUnwind returned %d\n", result);
  Fatal("FATAL forced transfer returned");
}

extern "C" __declspec(noinline) uint64_t CleanupGuestCallback(CleanupLink* link) {
  Resource resource(link, link->level * 100 + 70);
  if (state->kind == Normal) {
    // Ordinary round trip.
  } else if (link->level == 0 &&
             (state->kind == NestedOuter || state->kind == NearestInner)) {
    RunOwner(1);
  } else {
    Transfer(state->kind == NearestInner ? state->owners[1].token
                                         : state->owners[0].token);
  }
  Notify(state, CLEANUP_AFTER, link->level * 100 + 71, 1);
  return kResult;
}

extern "C" uint64_t gesher_guest_entry_trampoline(uint64_t fn, EntryFrame* frame) {
  Require(fn == reinterpret_cast<uint64_t>(&CleanupGuestCallback),
          "native callback identity through exact guest-entry thunk");
  auto* link = reinterpret_cast<CleanupLink*>(frame->gpr[0]);
  Resource resource(link, link->level * 100 + 60);
  const uint64_t result = CleanupGuestCallback(link);
  Notify(state, CLEANUP_AFTER, link->level * 100 + 61, 1);
  return result;
}

extern "C" __attribute__((sysv_abi)) __declspec(noinline)
uint64_t CleanupSysvTarget(CleanupLink* link, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
  Resource resource(link, link->level * 100 + 50);
  const uint64_t result = CleanupGuestEntry(link, 0, 0, 0, 0, 0, 0, 0, 0);
  Notify(state, CLEANUP_AFTER, link->level * 100 + 51, 1);
  return result;
}

__declspec(noinline) static uint64_t InvokeHost(CleanupLink* link) {
  Resource resource(link, link->level * 100 + 45);
  const uint64_t gpr[6] = {reinterpret_cast<uint64_t>(link), 0, 0, 0, 0, 0};
  const uint64_t xmm[8] = {};
  const uint64_t stack[3] = {};
  HostCall call{reinterpret_cast<void*>(&CleanupSysvTarget), gpr, xmm, stack, 3, 0, 0, 0};
  return gesher_call_host_sysv(&call);
}

__declspec(noinline) static uint64_t CatchHost(CleanupLink* link) {
  try {
    return InvokeHost(link);
  } catch (...) {
    ++state->catch_count;
    return kResult;
  }
}
__declspec(noinline) static uint64_t NoexceptHost(CleanupLink* link) noexcept {
  return InvokeHost(link);
}

static uint64_t NativeLoop(CleanupLink* link) {
  Resource resource(link, link->level * 100 + 40);
  uint64_t result = 0;
  if (state->mode == CatchAll) result = CatchHost(link);
  else if (state->mode == Noexcept) result = NoexceptHost(link);
  else if (state->mode == ForeignFinally || state->mode == ForeignNormal)
    result = CleanupForeignFinally(link, InvokeHost);
  else result = InvokeHost(link);
  Check(result == kResult, "normal native result preserved");
  Notify(state, CLEANUP_AFTER, link->level * 100 + 41, 1);
  return result;
}

static DWORD WINAPI RefuseForeign(void* opaque) {
  const Token token = *static_cast<Token*>(opaque);
  State local{};
  state = &local;
  // There is deliberately no Transfer call on this path.
  Check(!Live(token), "active foreign-thread token refused before transfer");
  return local.failures;
}

__declspec(noinline) static void RunOwner(unsigned level) {
  Require(state->depth == level && level < 2, "bounded owner nesting");
  Owner* const owner = &state->owners[level];
  *owner = {};
  owner->active = true;
  owner->parent = state->top;
  owner->token = {owner, static_cast<uint64_t>(InterlockedIncrement64(&next_generation)),
                  GetCurrentThreadId()};
  owner->link = {Notify, state, NativeLoop, static_cast<int>(level)};
  state->top = owner;
  ++state->depth;
  Check(Live(owner->token) && !Live(state->previous[level]),
        "active reused slot accepts only its current generation");
  Registration registration(owner);
  Resource resource(&owner->link, level * 100 + 10);
  owner->expected_resources = state->resource_depth;

  if (CleanupSave(&owner->checkpoint) == 0) {
    Require(_Unwind_Backtrace(TraceOwner, owner) == _URC_NORMAL_STOP,
            "capture the current owner through the actual LLVM unwinder");
    Require(owner->cfa == owner->checkpoint.rsp,
            "owner CFA equals the native checkpoint stack position");
    if (!state->worker && state->cases == 0 && level == 0) {
      Token token = owner->token;
      HANDLE thread = CreateThread(nullptr, 0, RefuseForeign, &token, 0, nullptr);
      Require(thread != nullptr, "create foreign-token validation thread");
      Require(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0,
              "foreign-token validation completes");
      DWORD exit_code = 1;
      Require(GetExitCodeThread(thread, &exit_code) && exit_code == 0,
              "foreign token refused without transfer");
      Require(CloseHandle(thread) != 0, "close foreign-token validation thread");
    }
    Check(dll_call(&owner->link) == kResult, "ordinary DLL callback result");
    ++owner->normal;
  } else {
    if (state->mode == RawRejected &&
        (state->resource_depth != owner->expected_resources || owner->stop_seen != 1))
      Fatal("REJECTED raw native jump skipped owned C++ cleanup; not a portable baseline", 74);
    Require(owner->stop_seen == 1 && owner->released == 1,
            "landing occurs after forced cleanup and exception release");
    Require(state->top == owner && state->resource_depth == owner->expected_resources,
            "destination retains only its still-live resources");
    ++owner->landed;
  }
}

static void OneCase(Kind kind) {
  Require(state->depth == 0 && state->resource_depth == 0, "fresh case has no live owner/resource");
  state->kind = kind;
  std::memset(state->after, 0, sizeof state->after);
  std::memset(state->destroyed_owners, 0, sizeof state->destroyed_owners);
  state->foreign_open = state->foreign_close = state->catch_count = 0;
  const unsigned created = state->created, destroyed = state->destroyed;
  const Token stale = state->previous[0];
  RunOwner(0);
  Check(state->top == nullptr && state->depth == 0 && state->resource_depth == 0,
        "all owners and resources retire after return");
  Check(state->created - created == state->destroyed - destroyed,
        "real resource construction and cleanup counts balance");
  Check(!Live(stale) && !Live(state->previous[0]), "expired/reused tokens refused before transfer");
  if (state->mode == CatchAll) {
    Require(state->catch_count == 1 && state->owners[0].requested &&
                state->owners[0].landed == 0 && state->owners[0].normal == 1 &&
                state->owners[0].released == 1,
            "catch-all characterization has a specific intercepted transfer");
    Fatal("UNSUPPORTED catch-all intercepted forced transfer", 71);
  }
  Check(state->destroyed_owners[0] == 1, "outer owner cleanup exactly once");
  const bool nested = kind == NestedOuter || kind == NearestInner;
  Check(state->destroyed_owners[1] == (nested ? 1u : 0u), "inner owner lifetime cleanup");
  const bool outer_returns = kind == Normal || kind == NearestInner;
  Check(state->owners[0].normal == (outer_returns ? 1u : 0u) &&
        state->owners[0].landed == (outer_returns ? 0u : 1u), "correct outer destination");
  if (nested)
    Check(state->owners[1].normal == 0 &&
          state->owners[1].landed == (kind == NearestInner ? 1u : 0u),
          "nearest target does not escape to the outer owner");
  for (unsigned id : {21u, 31u, 41u, 51u, 61u, 71u}) {
    Check(state->after[0][id] == (outer_returns ? 1u : 0u),
          "abandoned outer native continuations never execute");
    Check(state->after[1][id] == 0, "abandoned inner native continuations never execute");
  }
  if (state->mode == ForeignNormal)
    Check(state->foreign_open == 1 && state->foreign_close == 1,
          "foreign finally positive control executes on ordinary return");
  ++state->cases;
}

static DWORD WINAPI Worker(void* opaque) {
  state = static_cast<State*>(opaque);
  state->worker = true;
  for (unsigned kind = Normal; kind <= NearestInner; ++kind)
    OneCase(static_cast<Kind>(kind));
  return state->failures;
}

[[noreturn]] static void Terminated() {
  if (state != nullptr && state->mode == Noexcept && state->transfers != 0)
    Fatal("UNSUPPORTED noexcept boundary terminated forced transfer", 72);
  Fatal("FATAL unexpected native terminate");
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::set_terminate(Terminated);
  State main_state{};
  state = &main_state;
  if (argc == 2) {
    if (std::strcmp(argv[1], "--catch-all") == 0) state->mode = CatchAll;
    else if (std::strcmp(argv[1], "--noexcept") == 0) state->mode = Noexcept;
    else if (std::strcmp(argv[1], "--foreign-finally") == 0) state->mode = ForeignFinally;
    else if (std::strcmp(argv[1], "--raw-rejected") == 0) state->mode = RawRejected;
    else if (std::strcmp(argv[1], "--foreign-normal") == 0) state->mode = ForeignNormal;
    else Fatal("FATAL unknown mode");
  } else if (argc != 1) {
    Fatal("FATAL unexpected arguments");
  }
  wchar_t dll_path[32768];
  const DWORD length = GetModuleFileNameW(nullptr, dll_path, 32768);
  Require(length != 0 && length < 32768, "resolve native probe image");
  wchar_t* slash = std::wcsrchr(dll_path, L'\\');
  Require(slash != nullptr && slash - dll_path + 32 < 32768, "helper DLL path capacity");
  std::wcscpy(slash + 1, L"native_cleanup_helper.dll");
  HMODULE helper = LoadLibraryW(dll_path);
  Require(helper != nullptr, "load exact adjacent native helper DLL");
  dll_call = reinterpret_cast<decltype(dll_call)>(GetProcAddress(helper, "CleanupDllCall"));
  auto get_unwinder = reinterpret_cast<void* (*)()>(GetProcAddress(helper, "CleanupDllUnwinder"));
  Require(dll_call != nullptr && get_unwinder != nullptr, "native DLL exports");
  void* const exe_unwinder = reinterpret_cast<void*>(&_Unwind_ForcedUnwind);
  void* const dll_unwinder = get_unwinder();
  HMODULE exe_owner = nullptr, dll_owner = nullptr;
  Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(exe_unwinder), &exe_owner) &&
          GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(dll_unwinder), &dll_owner),
          "identify actual static unwind implementations");
  Require(exe_unwinder != dll_unwinder && exe_owner == GetModuleHandleW(nullptr) &&
              dll_owner == helper, "EXE and helper own separate pinned static unwind copies");
  std::printf("native cleanup: begin mode=%d; independent static EXE/DLL unwind copies\n", state->mode);
  if (state->mode != Supported) {
    OneCase(state->mode == ForeignNormal ? Normal : EscapeOwner);
  } else {
    OneCase(Normal);  // Initialize native runtime bookkeeping before leak measurement.
    DWORD handles_before = 0, handles_after = 0;
    Require(GetProcessHandleCount(GetCurrentProcess(), &handles_before),
            "read initial actual handle count");
    for (unsigned repeat = 0; repeat != 16; ++repeat)
      for (unsigned kind = Normal; kind <= NearestInner; ++kind)
        OneCase(static_cast<Kind>(kind));
    State workers[4]{};
    HANDLE threads[4]{};
    for (unsigned i = 0; i != 4; ++i) {
      threads[i] = CreateThread(nullptr, 0, Worker, &workers[i], 0, nullptr);
      Require(threads[i] != nullptr, "create independent native owner thread");
    }
    Require(WaitForMultipleObjects(4, threads, TRUE, 30000) == WAIT_OBJECT_0,
            "all native owner threads finish");
    for (unsigned i = 0; i != 4; ++i) {
      DWORD code = 1;
      Check(GetExitCodeThread(threads[i], &code) && code == 0, "native owner thread result");
      Require(CloseHandle(threads[i]), "close native owner thread");
      state->checks += workers[i].checks;
      state->failures += workers[i].failures;
      state->cases += workers[i].cases;
      state->created += workers[i].created;
      state->destroyed += workers[i].destroyed;
      state->stop_calls += workers[i].stop_calls;
      state->transfers += workers[i].transfers;
    }
    Require(GetProcessHandleCount(GetCurrentProcess(), &handles_after),
            "read final actual handle count");
    Check(handles_before == handles_after, "repeated/thread cases retain no native handles");
  }
  Require(FreeLibrary(helper), "release helper after every owner lifetime ends");
  std::printf("native callback cleanup: %s; cases=%u checks=%u transfers=%u "
              "stop-calls=%u resources=%u/%u failures=%u\n",
              state->failures ? "FAILED" : "passed", state->cases, state->checks,
              state->transfers, state->stop_calls, state->created, state->destroyed,
              state->failures);
  return state->failures == 0 ? 0 : 1;
}
