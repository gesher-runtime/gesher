// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "jit_windows.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_failures = 0;

static void report(const char* name, bool ok, const char* detail) {
  printf("%-20s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
  if (!ok) {
    g_failures++;
  }
}

// Target function conforming to Windows x64 ABI
static uint64_t __attribute__((ms_abi)) TargetWin64Func(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  return a * 1000 + b * 100 + c * 10 + d;
}

// Target function conforming to Linux System V ABI
static uint64_t __attribute__((sysv_abi)) TargetSysVFunc(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6;
}

static void ProbeCallingConventions() {
  using namespace art::jit::windows;

  // 1. Check Windows x64 register properties
  bool win_args_ok = (WindowsX64Abi::kNumArgGprs == 4) &&
                     (WindowsX64Abi::kArgGprs[0] == X86_64Reg::kRCX) &&
                     (WindowsX64Abi::kArgGprs[1] == X86_64Reg::kRDX) &&
                     (WindowsX64Abi::kArgGprs[2] == X86_64Reg::kR8) &&
                     (WindowsX64Abi::kArgGprs[3] == X86_64Reg::kR9) &&
                     (WindowsX64Abi::kShadowSpaceBytes == 32);

  bool win_callee_ok = WindowsX64Abi::IsCalleeSavedGpr(X86_64Reg::kRBX) &&
                       WindowsX64Abi::IsCalleeSavedGpr(X86_64Reg::kRBP) &&
                       WindowsX64Abi::IsCalleeSavedGpr(X86_64Reg::kRDI) &&
                       WindowsX64Abi::IsCalleeSavedGpr(X86_64Reg::kRSI) &&
                       WindowsX64Abi::IsCalleeSavedGpr(X86_64Reg::kRSP) &&
                       WindowsX64Abi::IsCalleeSavedGpr(X86_64Reg::kR12) &&
                       WindowsX64Abi::IsCalleeSavedGpr(X86_64Reg::kR13) &&
                       WindowsX64Abi::IsCalleeSavedGpr(X86_64Reg::kR14) &&
                       WindowsX64Abi::IsCalleeSavedGpr(X86_64Reg::kR15) &&
                       WindowsX64Abi::IsCalleeSavedXmm(6) &&
                       WindowsX64Abi::IsCalleeSavedXmm(15) &&
                       !WindowsX64Abi::IsCalleeSavedXmm(5);

  report("win64_abi_specs", win_args_ok && win_callee_ok,
         "RCX,RDX,R8,R9 args; 32B shadow space; RBX,RBP,RDI,RSI,R12-R15,XMM6-15 callee-saved");

  // 2. Check Linux System V register properties
  bool sysv_args_ok = (SystemVAbi::kNumArgGprs == 6) &&
                      (SystemVAbi::kArgGprs[0] == X86_64Reg::kRDI) &&
                      (SystemVAbi::kArgGprs[1] == X86_64Reg::kRSI) &&
                      (SystemVAbi::kArgGprs[2] == X86_64Reg::kRDX) &&
                      (SystemVAbi::kArgGprs[3] == X86_64Reg::kRCX) &&
                      (SystemVAbi::kArgGprs[4] == X86_64Reg::kR8) &&
                      (SystemVAbi::kArgGprs[5] == X86_64Reg::kR9) &&
                      (SystemVAbi::kShadowSpaceBytes == 0);

  bool sysv_callee_ok = SystemVAbi::IsCalleeSavedGpr(X86_64Reg::kRBX) &&
                        SystemVAbi::IsCalleeSavedGpr(X86_64Reg::kRBP) &&
                        !SystemVAbi::IsCalleeSavedGpr(X86_64Reg::kRDI) &&
                        !SystemVAbi::IsCalleeSavedGpr(X86_64Reg::kRSI) &&
                        (SystemVAbi::kNumCalleeSaveFprs == 0);

  report("sysv_abi_specs", sysv_args_ok && sysv_callee_ok,
         "RDI,RSI,RDX,RCX,R8,R9 args; 0B shadow; RBX,RBP,R12-R15 callee-saved (RDI,RSI scratch)");

  // 3. Test CallWin64FromSysV bridge thunk
  uint64_t win_res = CallWin64FromSysV(
      reinterpret_cast<void*>(TargetWin64Func), 1, 2, 3, 4);
  bool bridge_win_ok = (win_res == 1234);
  char detail_win[64];
  snprintf(detail_win, sizeof(detail_win), "invoked win64 func result=%llu (expected 1234)",
           static_cast<unsigned long long>(win_res));
  report("bridge_win64_call", bridge_win_ok, detail_win);

  // 4. Test CallSysVFromWin64 bridge thunk
  uint64_t sysv_res = CallSysVFromWin64(
      reinterpret_cast<void*>(TargetSysVFunc), 1, 2, 3, 4, 5, 6);
  // 1 + 2*2(4) + 3*3(9) + 4*4(16) + 5*5(25) + 6*6(36) = 91
  bool bridge_sysv_ok = (sysv_res == 91);
  char detail_sysv[64];
  snprintf(detail_sysv, sizeof(detail_sysv), "invoked sysv func result=%llu (expected 91)",
           static_cast<unsigned long long>(sysv_res));
  report("bridge_sysv_call", bridge_sysv_ok, detail_sysv);
}

static void ProbeLow4GAllocationAndProtection() {
  using namespace art::jit::windows;

  // Allocate 64 KiB executable code memory in low 4GiB
  constexpr size_t kAllocSize = 65536;
  void* code_mem = JitLow4GAllocator::Instance().Allocate(kAllocSize, PAGE_EXECUTE_READWRITE);

  bool allocated = (code_mem != nullptr);
  uintptr_t addr = reinterpret_cast<uintptr_t>(code_mem);
  bool in_low4g = (addr + kAllocSize <= JitLow4GAllocator::kLow4GBoundary);

  char alloc_detail[96];
  snprintf(alloc_detail, sizeof(alloc_detail), "allocated %zu bytes at %p (below 4GiB: %s)",
           kAllocSize, code_mem, in_low4g ? "yes" : "no");
  report("jit_low4g_alloc", allocated && in_low4g, alloc_detail);

  if (!allocated || !in_low4g) {
    return;
  }

  // Emit small machine code:
  // mov eax, 0x54321
  // ret
  // Machine code: B8 21 43 05 00 C3
  static const uint8_t kReturnCode[] = {0xB8, 0x21, 0x43, 0x05, 0x00, 0xC3};
  std::memcpy(code_mem, kReturnCode, sizeof(kReturnCode));

  // Transition memory protection to PAGE_EXECUTE_READ
  bool exec_ok = JitLow4GAllocator::Instance().MakeExecutable(code_mem, kAllocSize);
  report("jit_protect_exec", exec_ok, "VirtualProtect transition to PAGE_EXECUTE_READ + icache flush");

  // Call the generated code
  typedef uint32_t (*JitFunc)();
  JitFunc fn = reinterpret_cast<JitFunc>(reinterpret_cast<uintptr_t>(code_mem));
  uint32_t result = fn();
  bool exec_res_ok = (result == 0x54321);
  char run_detail[64];
  snprintf(run_detail, sizeof(run_detail), "executed code returned 0x%X (expected 0x54321)", result);
  report("jit_code_execute", exec_res_ok, run_detail);

  // Transition back to PAGE_EXECUTE_READWRITE via ScopedJitWriteProtection
  {
    ScopedJitWriteProtection write_scope(code_mem, kAllocSize);
    // Overwrite with mov eax, 0x1234; ret
    // Machine code: B8 34 12 00 00 C3
    static const uint8_t kNewCode[] = {0xB8, 0x34, 0x12, 0x00, 0x00, 0xC3};
    std::memcpy(code_mem, kNewCode, sizeof(kNewCode));
  }
  // Scope exited, automatically restored to executable
  uint32_t patched_result = fn();
  bool patch_ok = (patched_result == 0x1234);
  char patch_detail[64];
  snprintf(patch_detail, sizeof(patch_detail), "patched code returned 0x%X (expected 0x1234)", patched_result);
  report("jit_scoped_write", patch_ok, patch_detail);

  // Free memory
  bool free_ok = JitLow4GAllocator::Instance().Free(code_mem, kAllocSize);
  report("jit_low4g_free", free_ok, "VirtualFree released low 4GiB chunk");
}

static void ProbeSehUnwinding() {
  using namespace art::jit::windows;

  // Allocate chunk to register function table for
  constexpr size_t kChunkSize = 16384;
  void* chunk = JitLow4GAllocator::Instance().Allocate(kChunkSize);
  if (chunk == nullptr) {
    report("jit_seh_unwind", false, "failed to allocate memory for unwinding probe");
    return;
  }

  // Register code chunk with RtlAddFunctionTable
  bool reg_ok = JitSehUnwindRegistry::Instance().RegisterCodeChunk(chunk, kChunkSize, "ProbeChunk");
  report("jit_seh_register", reg_ok, "RtlAddFunctionTable registered code chunk with SEH runtime");

  // Unregister code chunk with RtlDeleteFunctionTable
  bool unreg_ok = JitSehUnwindRegistry::Instance().UnregisterCodeChunk(chunk);
  report("jit_seh_unregister", unreg_ok, "RtlDeleteFunctionTable unhooked code chunk");

  JitLow4GAllocator::Instance().Free(chunk, kChunkSize);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("=== Gesher ART JIT Windows x64 Subsystem Probe ===\n");

  ProbeCallingConventions();
  ProbeLow4GAllocationAndProtection();
  ProbeSehUnwinding();

  printf("\nProbe summary: %d failure(s)\n", g_failures);
  return g_failures;
}
