// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "arm64_unwind_offsets.h"  // Derived from the exact production object.

struct HostCall {
  void* fn;
  const uint64_t* gpr;
  const uint64_t* xmm;
  const uint64_t* stack;
  uint64_t nstack, out_xmm0, out_rdx;
};
struct EntryFrame {
  uint64_t gpr[8], xmm[8];
  const uint64_t* stack;
  uint64_t out_rdx, out_xmm0;
};
struct alignas(16) RegisterState {
  uint64_t gpr[4], xmm[20], rsp, rip;
};
struct NativeArgs {
  uint64_t gpr[6], xmm[8];
  const uint64_t* stack;
  uint64_t entry_rsp, vector_count;
};
struct Result { uint64_t rax, rdx, xmm0; };

static_assert(offsetof(HostCall, out_xmm0) == 40 && sizeof(HostCall) == 56);
static_assert(offsetof(EntryFrame, stack) == 128 && sizeof(EntryFrame) == 152);
static_assert(offsetof(RegisterState, xmm) == 32);
static_assert(offsetof(RegisterState, rsp) == 192 && sizeof(RegisterState) == 208);
static_assert(offsetof(NativeArgs, stack) == 112 && sizeof(NativeArgs) == 136);
static_assert(offsetof(CONTEXT, Xmm15) == offsetof(CONTEXT, Xmm6) + 9 * 16);

extern "C" {
uint64_t gesher_call_host_sysv(HostCall*);
void gesher_guest_entry_shim();
void UnwindSysvTarget();
uint64_t UnwindCallHost(HostCall*, RegisterState*, RegisterState*);
uint64_t UnwindCallGuest(RegisterState*, RegisterState*, Result*);
alignas(16) uint64_t unwind_seed_xmm[20];
uint64_t unwind_guest_gpr[9];
uint64_t unwind_fp_args[8];
uint64_t unwind_guest_function = UINT64_C(0x123456789abcdef0);
uint64_t unwind_result_fp = UINT64_C(0x400921fb54442d18);
}

static constexpr uint64_t kGpr[] = {
    UINT64_C(0x1122334455667788), UINT64_C(0x2233445566778899),
    UINT64_C(0x33445566778899aa), UINT64_C(0x445566778899aabb)};
static constexpr uint64_t kRax = UINT64_C(0x8192a3b4c5d6e7f8);
static constexpr uint64_t kRdx = UINT64_C(0x92a3b4c5d6e7f809);
static unsigned checks, failures, breakpoint_hits, live_unwinds;
static const RegisterState* expected;
static const HostCall* active_call;
static uint8_t* breakpoint_address;
static uint8_t breakpoint_original;
static bool breakpoint_active, breakpoint_host;
static unsigned current_offset;

static void Check(bool ok, const char* scope, const char* item) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL %s %s (offset 0x%x)\n", scope, item, current_offset);
  }
}

static void CheckWord(uint64_t got, uint64_t want, const char* scope,
                      const char* item) {
  Check(got == want, scope, item);
  if (got != want)
    std::printf("  actual=%016llx expected=%016llx\n",
                static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(want));
}

static uint64_t Address(bool host) {
  return reinterpret_cast<uint64_t>(
      host ? reinterpret_cast<void*>(&gesher_call_host_sysv)
           : reinterpret_cast<void*>(&gesher_guest_entry_shim));
}

static void CheckRecovered(const CONTEXT& context, bool host) {
  const char* scope = host ? "host unwind" : "guest unwind";
  CheckWord(context.Rip, expected->rip, scope, "caller PC");
  CheckWord(context.Rsp, expected->rsp, scope, "caller RSP");
  CheckWord(context.Rbp, expected->gpr[0], scope, "RBP");
  CheckWord(context.Rbx, expected->gpr[1], scope, "RBX");
  if (!host) return;  // System V does not preserve RSI/RDI or XMM registers.
  CheckWord(context.Rsi, expected->gpr[2], scope, "RSI");
  CheckWord(context.Rdi, expected->gpr[3], scope, "RDI");
  uint64_t xmm[20];
  std::memcpy(xmm, reinterpret_cast<const uint8_t*>(&context) +
                       offsetof(CONTEXT, Xmm6), sizeof xmm);
  for (unsigned i = 0; i != 10; ++i) {
    char name[24];
    std::snprintf(name, sizeof name, "xmm%u", i + 6);
    Check(std::memcmp(xmm + i * 2, expected->xmm + i * 2, 16) == 0,
          scope, name);
  }
}

static bool UnwindOne(CONTEXT& context, DWORD64* establisher = nullptr) {
  DWORD64 image = 0, frame = 0;
  auto* function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
  Check(function != nullptr, "native walk", "function entry");
  if (function == nullptr) return false;
  void* data = nullptr;
  RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context,
                   &data, &frame, nullptr);
  if (establisher != nullptr) *establisher = frame;
  return true;
}

// Capture actual native execution from inside each called target, walk through
// its real helper frames, then unwind the production thunk on a copy only.
__declspec(noinline) static void CaptureAndUnwind(bool host) {
  CONTEXT context{};
  RtlCaptureContext(&context);
  const uint64_t start = Address(host);
  const unsigned size = host ? kHostCodeSize : kGuestCodeSize;
  for (unsigned depth = 0; depth != 12; ++depth) {
    if (context.Rip >= start && context.Rip < start + size) {
      DWORD64 frame = 0;
      if (!UnwindOne(context, &frame)) return;
      CheckWord(frame, expected->rsp - (host ? 208 : 176),
                host ? "host unwind" : "guest unwind", "fixed frame base");
      CheckRecovered(context, host);
      ++live_unwinds;
      return;
    }
    if (!UnwindOne(context)) return;
  }
  Check(false, "native walk", "production thunk reached");
}

extern "C" void UnwindProbeHost(const NativeArgs* args) {
  Check((args->entry_rsp & 15) == 8, "host ABI", "callee stack alignment");
  Check(args->vector_count == 8, "host ABI", "variadic AL");
  for (unsigned i = 0; i != 6; ++i)
    CheckWord(args->gpr[i], active_call->gpr[i], "host ABI", "integer argument");
  for (unsigned i = 0; i != 8; ++i)
    CheckWord(args->xmm[i], active_call->xmm[i], "host ABI", "FP argument");
  for (unsigned i = 0; i != active_call->nstack; ++i)
    CheckWord(args->stack[i], active_call->stack[i], "host ABI", "stack argument");
  CaptureAndUnwind(true);
}

extern "C" uint64_t UnwindProbeGuest(uint64_t fn, EntryFrame* frame,
                                     uint64_t entry_rsp) {
  Check((entry_rsp & 15) == 8, "guest ABI", "Microsoft stack alignment");
  CheckWord(fn, unwind_guest_function, "guest ABI", "guest function identity");
  for (unsigned i = 0; i != 8; ++i) {
    CheckWord(frame->gpr[i], unwind_guest_gpr[i], "guest ABI", "integer argument");
    CheckWord(frame->xmm[i], unwind_fp_args[i], "guest ABI", "FP argument");
  }
  CheckWord(frame->stack[0], unwind_guest_gpr[8], "guest ABI", "argument nine");
  Check(frame->out_rdx == 0 && frame->out_xmm0 == 0,
        "guest ABI", "result slots initially clear");
  CaptureAndUnwind(false);
  frame->out_rdx = kRdx;
  frame->out_xmm0 = unwind_result_fp;
  return kRax;
}

static void RunHost(unsigned nstack) {
  RegisterState before{}, after{};
  uint64_t stack[31];
  for (unsigned i = 0; i != 31; ++i)
    stack[i] = UINT64_C(0x6172839400000000) + i * UINT64_C(0x10101);
  HostCall call{reinterpret_cast<void*>(&UnwindSysvTarget), unwind_guest_gpr,
                unwind_fp_args, stack, nstack, 0, 0};
  expected = &before;
  active_call = &call;
  const uint64_t result = UnwindCallHost(&call, &before, &after);
  CheckWord(result, kRax, "host ABI", "RAX result");
  CheckWord(call.out_rdx, kRdx, "host ABI", "RDX result");
  CheckWord(call.out_xmm0, unwind_result_fp, "host ABI", "XMM0 result");
  Check(std::memcmp(before.gpr, kGpr, sizeof kGpr) == 0,
        "host caller", "distinct GPR seeds");
  Check(std::memcmp(before.xmm, unwind_seed_xmm, sizeof before.xmm) == 0,
        "host caller", "distinct full-width XMM seeds");
  Check(std::memcmp(&before, &after, sizeof before) == 0,
        "host ABI", "ordinary preserved state and stack round trip");
  expected = nullptr;
  active_call = nullptr;
}

static void RunGuest() {
  RegisterState before{}, after{};
  Result result{};
  expected = &before;
  CheckWord(UnwindCallGuest(&before, &after, &result), kRax,
            "guest ABI", "wrapper RAX result");
  CheckWord(result.rax, kRax, "guest ABI", "RAX result");
  CheckWord(result.rdx, kRdx, "guest ABI", "RDX result");
  CheckWord(result.xmm0, unwind_result_fp, "guest ABI", "XMM0 result");
  for (unsigned i = 0; i != 2; ++i) {
    CheckWord(before.gpr[i], kGpr[i], "guest caller", "distinct GPR seed");
    CheckWord(after.gpr[i], before.gpr[i], "guest ABI", "preserved GPR");
  }
  CheckWord(after.rsp, before.rsp, "guest ABI", "stack round trip");
  expected = nullptr;
}

// A temporary software breakpoint observes a real instruction-boundary CONTEXT.
// Restore the original byte BEFORE RtlVirtualUnwind scans the epilog. Only the
// copied CONTEXT is unwound; execution resumes the unchanged original instruction.
static LONG CALLBACK Breakpoint(EXCEPTION_POINTERS* info) {
  if (!breakpoint_active ||
      info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
      info->ExceptionRecord->ExceptionAddress != breakpoint_address)
    return EXCEPTION_CONTINUE_SEARCH;
  *breakpoint_address = breakpoint_original;
  Check(FlushInstructionCache(GetCurrentProcess(), breakpoint_address, 1) != 0,
        "breakpoint", "restore instruction cache");
  breakpoint_active = false;
  info->ContextRecord->Rip = reinterpret_cast<DWORD64>(breakpoint_address);
  CONTEXT copy = *info->ContextRecord;
  if (UnwindOne(copy)) CheckRecovered(copy, breakpoint_host);
  ++breakpoint_hits;
  return EXCEPTION_CONTINUE_EXECUTION;
}

static void RunBoundary(bool host, unsigned offset, unsigned nstack = 0) {
  current_offset = offset;
  breakpoint_address = reinterpret_cast<uint8_t*>(Address(host)) + offset;
  breakpoint_original = *breakpoint_address;
  breakpoint_host = host;
  DWORD protection = 0;
  if (!VirtualProtect(breakpoint_address, 1, PAGE_EXECUTE_READWRITE, &protection)) {
    Check(false, "breakpoint", "make instruction writable");
    return;
  }
  const unsigned old_hits = breakpoint_hits;
  breakpoint_active = true;
  *breakpoint_address = 0xcc;
  Check(FlushInstructionCache(GetCurrentProcess(), breakpoint_address, 1) != 0,
        "breakpoint", "install instruction cache");
  if (host) RunHost(nstack); else RunGuest();
  // Also restore after a missed boundary, so the next test cannot inherit it.
  *breakpoint_address = breakpoint_original;
  breakpoint_active = false;
  Check(FlushInstructionCache(GetCurrentProcess(), breakpoint_address, 1) != 0,
        "breakpoint", "final instruction cache");
  DWORD ignored = 0;
  Check(VirtualProtect(breakpoint_address, 1, protection, &ignored) != 0,
        "breakpoint", "restore page protection");
  Check(breakpoint_hits == old_hits + 1, "breakpoint", "one actual boundary hit");
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  for (unsigned i = 0; i != 20; ++i)
    unwind_seed_xmm[i] = UINT64_C(0x1020304050607080) ^
                        (UINT64_C(0x0102040810204081) * (i + 1));
  for (unsigned i = 0; i != 9; ++i)
    unwind_guest_gpr[i] = UINT64_C(0x2345678900000000) + i * UINT64_C(0x10305);
  for (unsigned i = 0; i != 8; ++i)
    unwind_fp_args[i] = UINT64_C(0x3ff1000000000000) + i * UINT64_C(0x1000000000000);

  // An object with the old missing metadata must fail here, before attempting
  // to walk its nonleaf frames as if they were leaves.
  for (bool host : {true, false}) {
    DWORD64 image = 0;
    auto* entry = RtlLookupFunctionEntry(Address(host), &image, nullptr);
    if (entry == nullptr || image + entry->BeginAddress != Address(host)) {
      std::printf("FAIL missing production unwind metadata: %s\n",
                  host ? "gesher_call_host_sysv" : "gesher_guest_entry_shim");
      return 2;
    }
    Check(entry->EndAddress - entry->BeginAddress ==
              (host ? kHostCodeSize : kGuestCodeSize),
          "metadata", "exact production function range");
  }
  for (unsigned nstack : {0u, 1u, 2u, 16u, 31u}) RunHost(nstack);
  RunGuest();
  if (failures != 0) return 1;

  void* handler = AddVectoredExceptionHandler(1, Breakpoint);
  Check(handler != nullptr, "breakpoint", "install native handler");
  if (handler == nullptr) return 1;
  for (unsigned nstack : {0u, 1u, 2u, 16u, 31u})
    for (unsigned offset : kHostBoundaries) RunBoundary(true, offset, nstack);
  for (unsigned offset : kGuestBoundaries) RunBoundary(false, offset);
  Check(RemoveVectoredExceptionHandler(handler) != 0,
        "breakpoint", "remove native handler");
  std::printf("arm64 native thunk unwind: %s; checks=%u live-unwinds=%u "
              "instruction-boundaries=%u failures=%u\n",
              failures ? "FAILED" : "passed", checks, live_unwinds,
              breakpoint_hits, failures);
  return failures == 0 ? 0 : 1;
}
