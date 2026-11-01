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

#ifndef GESHER_COMPAT_ART_JIT_WINDOWS_H_
#define GESHER_COMPAT_ART_JIT_WINDOWS_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace art {
namespace jit {
namespace windows {

// ============================================================================
// 1. Calling Convention Specifications: Windows x64 vs Linux System V AMD64
// ============================================================================

enum class X86_64Reg : uint8_t {
  kRAX = 0,
  kRCX = 1,
  kRDX = 2,
  kRBX = 3,
  kRSP = 4,
  kRBP = 5,
  kRSI = 6,
  kRDI = 7,
  kR8  = 8,
  kR9  = 9,
  kR10 = 10,
  kR11 = 11,
  kR12 = 12,
  kR13 = 13,
  kR14 = 14,
  kR15 = 15,
  kNone = 255
};

// Windows x64 ABI calling convention:
// - Argument registers: RCX, RDX, R8, R9 (GPRs) and XMM0-XMM3 (FPRs).
// - Callee-saved registers: RBX, RBP, RDI, RSI, RSP, R12-R15, XMM6-XMM15.
// - Shadow space: 32 bytes (0x20) allocated by caller on stack before call.
struct WindowsX64Abi {
  static constexpr size_t kNumArgGprs = 4;
  static constexpr X86_64Reg kArgGprs[kNumArgGprs] = {
      X86_64Reg::kRCX, X86_64Reg::kRDX, X86_64Reg::kR8, X86_64Reg::kR9
  };

  static constexpr size_t kNumArgFprs = 4; // XMM0 - XMM3
  static constexpr size_t kShadowSpaceBytes = 32;
  static constexpr size_t kStackAlignment = 16;

  static constexpr size_t kNumCalleeSaveGprs = 9;
  static constexpr X86_64Reg kCalleeSaveGprs[kNumCalleeSaveGprs] = {
      X86_64Reg::kRBX, X86_64Reg::kRBP, X86_64Reg::kRDI, X86_64Reg::kRSI,
      X86_64Reg::kRSP, X86_64Reg::kR12, X86_64Reg::kR13, X86_64Reg::kR14,
      X86_64Reg::kR15
  };

  static constexpr size_t kNumCalleeSaveFprs = 10; // XMM6 - XMM15

  static bool IsCalleeSavedGpr(X86_64Reg reg);
  static bool IsCalleeSavedXmm(uint8_t xmm_idx);
  static bool IsArgGpr(X86_64Reg reg, size_t* arg_index = nullptr);
  static X86_64Reg GetArgGpr(size_t index);
};

// Linux System V AMD64 ABI calling convention:
// - Argument registers: RDI, RSI, RDX, RCX, R8, R9 (GPRs) and XMM0-XMM7 (FPRs).
// - Callee-saved registers: RBX, RBP, RSP, R12-R15 (RDI and RSI are caller-saved scratch!).
// - Shadow space: 0 bytes.
struct SystemVAbi {
  static constexpr size_t kNumArgGprs = 6;
  static constexpr X86_64Reg kArgGprs[kNumArgGprs] = {
      X86_64Reg::kRDI, X86_64Reg::kRSI, X86_64Reg::kRDX,
      X86_64Reg::kRCX, X86_64Reg::kR8,  X86_64Reg::kR9
  };

  static constexpr size_t kNumArgFprs = 8; // XMM0 - XMM7
  static constexpr size_t kShadowSpaceBytes = 0;
  static constexpr size_t kStackAlignment = 16;

  static constexpr size_t kNumCalleeSaveGprs = 7;
  static constexpr X86_64Reg kCalleeSaveGprs[kNumCalleeSaveGprs] = {
      X86_64Reg::kRBX, X86_64Reg::kRBP, X86_64Reg::kRSP,
      X86_64Reg::kR12, X86_64Reg::kR13, X86_64Reg::kR14,
      X86_64Reg::kR15
  };

  static constexpr size_t kNumCalleeSaveFprs = 0; // All XMM registers are scratch

  static bool IsCalleeSavedGpr(X86_64Reg reg);
  static bool IsArgGpr(X86_64Reg reg, size_t* arg_index = nullptr);
  static X86_64Reg GetArgGpr(size_t index);
};

// ABI Bridge thunk routines
uint64_t CallWin64FromSysV(void* target, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
uint64_t CallSysVFromWin64(void* target, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5 = 0, uint64_t a6 = 0);


// ============================================================================
// 2. Structured Exception Handling (SEH) & Stack Unwinding for JIT Frames
// ============================================================================

#pragma pack(push, 1)

// Operation codes for unwind codes
enum UnwindOpCodes : uint8_t {
  kUwopPushNonVolReg = 0,
  kUwopAllocLarge = 1,
  kUwopAllocSmall = 2,
  kUwopSetFpReg = 3,
  kUwopSaveNonVolReg = 4,
  kUwopSaveNonVolRegFar = 5,
  kUwopSaveXmm128 = 8,
  kUwopSaveXmm128Far = 9,
  kUwopPushMachFrame = 10
};

// 16-bit unwind code union
union UnwindCode {
  struct {
    uint8_t code_offset;
    uint8_t unwind_op : 4;
    uint8_t op_info : 4;
  };
  uint16_t frame_offset;
};

// Windows x64 UNWIND_INFO layout
struct UnwindInfo {
  uint8_t version : 3;
  uint8_t flags : 5;
  uint8_t size_of_prolog;
  uint8_t count_of_codes;
  uint8_t frame_register : 4;
  uint8_t frame_offset : 4;
  UnwindCode unwind_codes[1]; // Flexible array sized by count_of_codes
};

#pragma pack(pop)

// Structure holding registered function table data for a JIT code region
struct RegisteredFunctionTable {
  void* base_address = nullptr;
  size_t size = 0;
  std::vector<RUNTIME_FUNCTION> function_table;
  std::vector<uint8_t> unwind_info_buffer;
  bool is_registered = false;
  std::string tag;
};

class JitSehUnwindRegistry {
 public:
  static JitSehUnwindRegistry& Instance();

  // Register an entire generated code chunk with RtlAddFunctionTable
  bool RegisterCodeChunk(void* base_address, size_t size, const char* name = nullptr);

  // Register a specific compiled method within a code chunk
  bool RegisterCompiledMethod(void* code_start, size_t code_size, size_t frame_size,
                              bool has_frame_pointer = true);

  // Unregister a code chunk by its base address
  bool UnregisterCodeChunk(void* base_address);

  // Unregister all registered code chunks (called on JIT shutdown)
  void UnregisterAll();

  size_t GetRegisteredChunkCount() const;

 private:
  JitSehUnwindRegistry() = default;
  ~JitSehUnwindRegistry();

  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<RegisteredFunctionTable>> tables_;
};


// ============================================================================
// 3. Low 4GiB Executable Memory Allocator with VirtualProtect Transitions
// ============================================================================

enum class JitProtection {
  kReadWrite,
  kReadExecute,
  kExecuteReadWrite,
  kNoAccess
};

struct Low4GAllocation {
  void* address = nullptr;
  size_t size = 0;
  DWORD current_protection = PAGE_NOACCESS;
};

class JitLow4GAllocator {
 public:
  static constexpr uintptr_t kLow4GBoundary = 0x100000000ULL; // 4 GiB
  static constexpr uintptr_t kPreferredBase = 0x40000000ULL;   // 1 GiB

  static JitLow4GAllocator& Instance();

  // Allocate executable memory strictly within low 4GiB address space
  void* Allocate(size_t size, DWORD initial_protect = PAGE_EXECUTE_READWRITE);

  // Protect memory region with VirtualProtect
  bool Protect(void* addr, size_t size, DWORD new_protect, DWORD* old_protect = nullptr);

  // Transition to PAGE_EXECUTE_READ for safe execution
  bool MakeExecutable(void* addr, size_t size);

  // Transition to PAGE_EXECUTE_READWRITE for compilation/patching
  bool MakeWritable(void* addr, size_t size);

  // Free allocated memory
  bool Free(void* addr, size_t size);

  // Clean up all allocations
  void FreeAll();

  size_t GetAllocationCount() const;

 private:
  JitLow4GAllocator() = default;
  ~JitLow4GAllocator();

  mutable std::mutex mutex_;
  std::vector<Low4GAllocation> allocations_;
};

// RAII helper to temporarily switch code cache to writable and restore to executable
class ScopedJitWriteProtection {
 public:
  ScopedJitWriteProtection(void* addr, size_t size)
      : addr_(addr), size_(size), transitioned_(false) {
    if (addr_ != nullptr && size_ > 0) {
      transitioned_ = JitLow4GAllocator::Instance().MakeWritable(addr_, size_);
    }
  }

  ~ScopedJitWriteProtection() {
    if (transitioned_) {
      JitLow4GAllocator::Instance().MakeExecutable(addr_, size_);
    }
  }

 private:
  void* addr_;
  size_t size_;
  bool transitioned_;
};

// ============================================================================
// 4. Initialization and Lifecycle Hooks
// ============================================================================

bool InitializeJitWindows(size_t initial_capacity = 0, size_t max_capacity = 0);
void ShutdownJitWindows();

}  // namespace windows
}  // namespace jit
}  // namespace art

// ============================================================================
// 5. C Linkage Interface for ART Runtime and Compiler Hooks
// ============================================================================

extern "C" {
void* art_jit_windows_alloc_low4g(size_t size);
bool  art_jit_windows_free_low4g(void* ptr, size_t size);
bool  art_jit_windows_protect(void* ptr, size_t size, DWORD new_protect, DWORD* old_protect);
bool  art_jit_windows_make_executable(void* ptr, size_t size);
bool  art_jit_windows_make_writable(void* ptr, size_t size);
bool  art_jit_windows_register_unwind(void* base_addr, size_t size, const char* name);
bool  art_jit_windows_unregister_unwind(void* base_addr);
bool  art_jit_windows_init();
void  art_jit_windows_shutdown();
uint64_t art_jit_call_sysv_from_win64(void* fn, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
uint64_t art_jit_call_win64_from_sysv(void* fn, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
}

#endif  // GESHER_COMPAT_ART_JIT_WINDOWS_H_
