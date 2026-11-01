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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace art {
namespace jit {
namespace windows {

// Dynamic VirtualAlloc2 definitions for runtime resolution
typedef PVOID(WINAPI* PfnVirtualAlloc2)(
    HANDLE Process,
    PVOID BaseAddress,
    SIZE_T Size,
    ULONG AllocationType,
    ULONG PageProtection,
    MEM_EXTENDED_PARAMETER* ExtendedParameters,
    ULONG ParameterCount);

static PfnVirtualAlloc2 GetVirtualAlloc2Fn() {
  static PfnVirtualAlloc2 fn = nullptr;
  static bool resolved = false;
  if (!resolved) {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase != nullptr) {
      fn = reinterpret_cast<PfnVirtualAlloc2>(
          reinterpret_cast<void*>(GetProcAddress(kernelbase, "VirtualAlloc2")));
    }
    if (fn == nullptr) {
      HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
      if (kernel32 != nullptr) {
        fn = reinterpret_cast<PfnVirtualAlloc2>(
            reinterpret_cast<void*>(GetProcAddress(kernel32, "VirtualAlloc2")));
      }
    }
    resolved = true;
  }
  return fn;
}

// Default SEH Exception Handler for JIT frames to enable Windows SEH stack traversal
extern "C" EXCEPTION_DISPOSITION ArtJitSehHandler(
    PEXCEPTION_RECORD exception_record,
    PVOID establisher_frame,
    PCONTEXT context_record,
    PVOID dispatcher_context) {
  (void)exception_record;
  (void)establisher_frame;
  (void)context_record;
  (void)dispatcher_context;
  // Let vectored handlers / ART libsigchain / SEH continue search
  return ExceptionContinueSearch;
}

// ============================================================================
// 1. Calling Convention Implementations
// ============================================================================

bool WindowsX64Abi::IsCalleeSavedGpr(X86_64Reg reg) {
  for (size_t i = 0; i < kNumCalleeSaveGprs; ++i) {
    if (kCalleeSaveGprs[i] == reg) {
      return true;
    }
  }
  return false;
}

bool WindowsX64Abi::IsCalleeSavedXmm(uint8_t xmm_idx) {
  return xmm_idx >= 6 && xmm_idx <= 15;
}

bool WindowsX64Abi::IsArgGpr(X86_64Reg reg, size_t* arg_index) {
  for (size_t i = 0; i < kNumArgGprs; ++i) {
    if (kArgGprs[i] == reg) {
      if (arg_index != nullptr) {
        *arg_index = i;
      }
      return true;
    }
  }
  return false;
}

X86_64Reg WindowsX64Abi::GetArgGpr(size_t index) {
  if (index < kNumArgGprs) {
    return kArgGprs[index];
  }
  return X86_64Reg::kNone;
}

bool SystemVAbi::IsCalleeSavedGpr(X86_64Reg reg) {
  for (size_t i = 0; i < kNumCalleeSaveGprs; ++i) {
    if (kCalleeSaveGprs[i] == reg) {
      return true;
    }
  }
  return false;
}

bool SystemVAbi::IsArgGpr(X86_64Reg reg, size_t* arg_index) {
  for (size_t i = 0; i < kNumArgGprs; ++i) {
    if (kArgGprs[i] == reg) {
      if (arg_index != nullptr) {
        *arg_index = i;
      }
      return true;
    }
  }
  return false;
}

X86_64Reg SystemVAbi::GetArgGpr(size_t index) {
  if (index < kNumArgGprs) {
    return kArgGprs[index];
  }
  return X86_64Reg::kNone;
}

// Bridge thunk: Call Windows x64 function from System V caller
// - Caller provides args in System V order: a1 (RDI), a2 (RSI), a3 (RDX), a4 (RCX).
// - Callee expects args in Windows x64 order: RCX, RDX, R8, R9.
// - 32 bytes shadow space must be allocated on stack immediately before call.
// - Stack aligned to 16 bytes prior to call.
uint64_t CallWin64FromSysV(void* target, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
  uint64_t result = 0;
#if defined(__x86_64__)
  __asm__ volatile(
      "subq $40, %%rsp\n\t"        // 32-byte shadow space + 8-byte alignment
      "movq %1, %%r10\n\t"         // target function pointer
      "movq %2, %%rcx\n\t"         // Windows arg1 = a1
      "movq %3, %%rdx\n\t"         // Windows arg2 = a2
      "movq %4, %%r8\n\t"          // Windows arg3 = a3
      "movq %5, %%r9\n\t"          // Windows arg4 = a4
      "callq *%%r10\n\t"
      "addq $40, %%rsp\n\t"
      : "=a"(result)
      : "r"(target), "r"(a1), "r"(a2), "r"(a3), "r"(a4)
      : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
#else
  (void)target; (void)a1; (void)a2; (void)a3; (void)a4;
#endif
  return result;
}

// Bridge thunk: Call System V function from Windows x64 caller
// - Preserves Windows callee-saved registers: RDI, RSI, XMM6-XMM15.
// - Maps args from Windows registers (RCX, RDX, R8, R9) to System V (RDI, RSI, RDX, RCX, R8, R9).
// - Aligns stack to 16 bytes without shadow space.
uint64_t CallSysVFromWin64(void* target, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5, uint64_t a6) {
  uint64_t result = 0;
#if defined(__x86_64__)
  __asm__ volatile(
      "pushq %%rdi\n\t"
      "pushq %%rsi\n\t"
      "subq $168, %%rsp\n\t"       // 160 bytes for XMM6-XMM15 + 8 bytes padding
      "movdqu %%xmm6, 0(%%rsp)\n\t"
      "movdqu %%xmm7, 16(%%rsp)\n\t"
      "movdqu %%xmm8, 32(%%rsp)\n\t"
      "movdqu %%xmm9, 48(%%rsp)\n\t"
      "movdqu %%xmm10, 64(%%rsp)\n\t"
      "movdqu %%xmm11, 80(%%rsp)\n\t"
      "movdqu %%xmm12, 96(%%rsp)\n\t"
      "movdqu %%xmm13, 112(%%rsp)\n\t"
      "movdqu %%xmm14, 128(%%rsp)\n\t"
      "movdqu %%xmm15, 144(%%rsp)\n\t"
      "movq %1, %%r11\n\t"         // target function pointer
      "movq %2, %%rdi\n\t"         // System V arg1 = a1
      "movq %3, %%rsi\n\t"         // System V arg2 = a2
      "movq %4, %%rdx\n\t"         // System V arg3 = a3
      "movq %5, %%rcx\n\t"         // System V arg4 = a4
      "movq %6, %%r8\n\t"          // System V arg5 = a5
      "movq %7, %%r9\n\t"          // System V arg6 = a6
      "callq *%%r11\n\t"
      "movdqu 0(%%rsp), %%xmm6\n\t"
      "movdqu 16(%%rsp), %%xmm7\n\t"
      "movdqu 32(%%rsp), %%xmm8\n\t"
      "movdqu 48(%%rsp), %%xmm9\n\t"
      "movdqu 64(%%rsp), %%xmm10\n\t"
      "movdqu 80(%%rsp), %%xmm11\n\t"
      "movdqu 96(%%rsp), %%xmm12\n\t"
      "movdqu 112(%%rsp), %%xmm13\n\t"
      "movdqu 128(%%rsp), %%xmm14\n\t"
      "movdqu 144(%%rsp), %%xmm15\n\t"
      "addq $168, %%rsp\n\t"
      "popq %%rsi\n\t"
      "popq %%rdi\n\t"
      : "=a"(result)
      : "r"(target), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6)
      : "rcx", "rdx", "r8", "r9", "r11", "memory");
#else
  (void)target; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
#endif
  return result;
}

// ============================================================================
// 2. Structured Exception Handling (SEH) & Stack Unwinding
// ============================================================================

JitSehUnwindRegistry& JitSehUnwindRegistry::Instance() {
  static JitSehUnwindRegistry instance;
  return instance;
}

JitSehUnwindRegistry::~JitSehUnwindRegistry() {
  UnregisterAll();
}

bool JitSehUnwindRegistry::RegisterCodeChunk(void* base_address, size_t size, const char* name) {
  if (base_address == nullptr || size == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // Check if chunk is already registered
  for (const auto& entry : tables_) {
    if (entry->base_address == base_address) {
      return true;
    }
  }

  auto table_entry = std::make_unique<RegisteredFunctionTable>();
  table_entry->base_address = base_address;
  table_entry->size = size;
  table_entry->tag = (name != nullptr) ? name : "JitCodeChunk";

  // Build UNWIND_INFO structure:
  // Version: 1
  // Flags: UNWIND_FLAG_EHANDLER (0x01) with ArtJitSehHandler
  // Prolog: 8 bytes (push rbp; mov rbp, rsp)
  // Unwind codes:
  //   1) UWOP_SET_FPREG (RBP as frame pointer)
  //   2) UWOP_PUSH_NONVOLREG (RBP)
  constexpr uint8_t kUnwindVersion = 1;
  constexpr uint8_t kUnwindFlagEHandler = 0x01;
  constexpr uint8_t kNumUnwindCodes = 2;

  // Layout:
  // UnwindInfo (4 bytes) + 2 * UnwindCode (4 bytes) = 8 bytes
  // ExceptionHandler RVA (4 bytes) + ExceptionData (4 bytes) = 8 bytes
  // Total = 16 bytes (aligned to 4 bytes)
  table_entry->unwind_info_buffer.resize(16, 0);
  uint8_t* p = table_entry->unwind_info_buffer.data();

  UnwindInfo* info = reinterpret_cast<UnwindInfo*>(p);
  info->version = kUnwindVersion;
  info->flags = kUnwindFlagEHandler;
  info->size_of_prolog = 8;
  info->count_of_codes = kNumUnwindCodes;
  info->frame_register = 5; // RBP
  info->frame_offset = 0;

  // Code 0: Set frame pointer (CodeOffset = 4, Op = UWOP_SET_FPREG, OpInfo = 0)
  info->unwind_codes[0].code_offset = 4;
  info->unwind_codes[0].unwind_op = kUwopSetFpReg;
  info->unwind_codes[0].op_info = 0;

  // Code 1: Push non-volatile register (CodeOffset = 1, Op = UWOP_PUSH_NONVOLREG, OpInfo = 5 (RBP))
  info->unwind_codes[1].code_offset = 1;
  info->unwind_codes[1].unwind_op = kUwopPushNonVolReg;
  info->unwind_codes[1].op_info = 5;

  // Exception handler RVA (relative to base_address):
  // We place the exception handler pointer
  uint32_t handler_rva = static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(ArtJitSehHandler)) -
      reinterpret_cast<uintptr_t>(base_address));
  std::memcpy(p + 8, &handler_rva, sizeof(uint32_t));

  // Build single RUNTIME_FUNCTION covering the entire code chunk
  RUNTIME_FUNCTION rf{};
  rf.BeginAddress = 0;
  rf.EndAddress = static_cast<DWORD>(size);
  // UnwindData is RVA of UnwindInfo; we store UnwindInfo at the beginning of the buffer
  // For dynamic registration, RtlAddFunctionTable accepts UnwindData as RVA relative to BaseAddress.
  // We point to our buffer RVA relative to BaseAddress:
  rf.UnwindData = static_cast<DWORD>(
      reinterpret_cast<uintptr_t>(table_entry->unwind_info_buffer.data()) -
      reinterpret_cast<uintptr_t>(base_address));

  table_entry->function_table.push_back(rf);

  BOOLEAN ok = RtlAddFunctionTable(
      table_entry->function_table.data(),
      static_cast<DWORD>(table_entry->function_table.size()),
      reinterpret_cast<DWORD64>(base_address));

  if (!ok) {
    return false;
  }

  table_entry->is_registered = true;
  tables_.push_back(std::move(table_entry));
  return true;
}

bool JitSehUnwindRegistry::RegisterCompiledMethod(void* code_start, size_t code_size,
                                                 size_t frame_size, bool has_frame_pointer) {
  if (code_start == nullptr || code_size == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  auto table_entry = std::make_unique<RegisteredFunctionTable>();
  table_entry->base_address = code_start;
  table_entry->size = code_size;
  table_entry->tag = "JitCompiledMethod";

  table_entry->unwind_info_buffer.resize(16, 0);
  uint8_t* p = table_entry->unwind_info_buffer.data();

  UnwindInfo* info = reinterpret_cast<UnwindInfo*>(p);
  info->version = 1;
  info->flags = 0; // No handler
  info->size_of_prolog = 8;

  if (has_frame_pointer) {
    info->count_of_codes = 2;
    info->frame_register = 5; // RBP
    info->frame_offset = 0;
    info->unwind_codes[0].code_offset = 4;
    info->unwind_codes[0].unwind_op = kUwopSetFpReg;
    info->unwind_codes[0].op_info = 0;
    info->unwind_codes[1].code_offset = 1;
    info->unwind_codes[1].unwind_op = kUwopPushNonVolReg;
    info->unwind_codes[1].op_info = 5;
  } else {
    // Fixed stack allocation without frame pointer
    info->count_of_codes = 1;
    info->frame_register = 0;
    info->frame_offset = 0;
    info->unwind_codes[0].code_offset = 4;
    if (frame_size <= 128 && frame_size > 0) {
      info->unwind_codes[0].unwind_op = kUwopAllocSmall;
      info->unwind_codes[0].op_info = static_cast<uint8_t>((frame_size / 8) - 1);
    } else {
      info->unwind_codes[0].unwind_op = kUwopAllocLarge;
      info->unwind_codes[0].op_info = 0;
    }
  }

  RUNTIME_FUNCTION rf{};
  rf.BeginAddress = 0;
  rf.EndAddress = static_cast<DWORD>(code_size);
  rf.UnwindData = static_cast<DWORD>(
      reinterpret_cast<uintptr_t>(table_entry->unwind_info_buffer.data()) -
      reinterpret_cast<uintptr_t>(code_start));

  table_entry->function_table.push_back(rf);

  BOOLEAN ok = RtlAddFunctionTable(
      table_entry->function_table.data(),
      static_cast<DWORD>(table_entry->function_table.size()),
      reinterpret_cast<DWORD64>(code_start));

  if (!ok) {
    return false;
  }

  table_entry->is_registered = true;
  tables_.push_back(std::move(table_entry));
  return true;
}

bool JitSehUnwindRegistry::UnregisterCodeChunk(void* base_address) {
  if (base_address == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = tables_.begin(); it != tables_.end(); ++it) {
    if ((*it)->base_address == base_address) {
      if ((*it)->is_registered) {
        RtlDeleteFunctionTable((*it)->function_table.data());
        (*it)->is_registered = false;
      }
      tables_.erase(it);
      return true;
    }
  }
  return false;
}

void JitSehUnwindRegistry::UnregisterAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : tables_) {
    if (entry->is_registered) {
      RtlDeleteFunctionTable(entry->function_table.data());
      entry->is_registered = false;
    }
  }
  tables_.clear();
}

size_t JitSehUnwindRegistry::GetRegisteredChunkCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tables_.size();
}

// ============================================================================
// 3. Low 4GiB Executable Memory Allocator with VirtualProtect Transitions
// ============================================================================

JitLow4GAllocator& JitLow4GAllocator::Instance() {
  static JitLow4GAllocator instance;
  return instance;
}

JitLow4GAllocator::~JitLow4GAllocator() {
  FreeAll();
}

void* JitLow4GAllocator::Allocate(size_t size, DWORD initial_protect) {
  if (size == 0) {
    return nullptr;
  }

  // Round up size to page boundary (4 KiB)
  constexpr size_t kPageSize = 4096;
  size = (size + kPageSize - 1) & ~(kPageSize - 1);

  std::lock_guard<std::mutex> lock(mutex_);

  void* allocated_ptr = nullptr;

  // Attempt 1: Modern VirtualAlloc2 with address requirements in low 4GiB space
  PfnVirtualAlloc2 pfnVirtualAlloc2 = GetVirtualAlloc2Fn();
  if (pfnVirtualAlloc2 != nullptr) {
    MEM_ADDRESS_REQUIREMENTS reqs{};
    reqs.LowestStartingAddress = reinterpret_cast<PVOID>(kPreferredBase);
    reqs.HighestEndingAddress = reinterpret_cast<PVOID>(kLow4GBoundary - 1);
    reqs.Alignment = 0; // System default (64 KiB)

    MEM_EXTENDED_PARAMETER param{};
    param.Type = MemExtendedParameterAddressRequirements;
    param.Pointer = &reqs;

    allocated_ptr = pfnVirtualAlloc2(
        GetCurrentProcess(),
        nullptr,
        size,
        MEM_RESERVE | MEM_COMMIT,
        initial_protect,
        &param,
        1);
  }

  // Attempt 2: Probing across low 4GiB address range in 64 KiB allocation granularity
  if (allocated_ptr == nullptr) {
    constexpr uintptr_t kAllocGranularity = 0x10000; // 64 KiB
    for (uintptr_t addr = kPreferredBase; addr + size < kLow4GBoundary; addr += kAllocGranularity) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        continue;
      }

      if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
        void* candidate = reinterpret_cast<void*>(addr);
        void* p = VirtualAlloc(candidate, size, MEM_RESERVE | MEM_COMMIT, initial_protect);
        if (p != nullptr) {
          if (reinterpret_cast<uintptr_t>(p) + size <= kLow4GBoundary) {
            allocated_ptr = p;
            break;
          } else {
            VirtualFree(p, 0, MEM_RELEASE);
          }
        }
      }

      uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      if (region_end > addr) {
        addr = (region_end + kAllocGranularity - 1) & ~(kAllocGranularity - 1);
        addr -= kAllocGranularity; // loop will add kAllocGranularity
      }
    }
  }

  // Verify allocated memory is strictly within low 4GiB
  if (allocated_ptr != nullptr) {
    uintptr_t end_addr = reinterpret_cast<uintptr_t>(allocated_ptr) + size;
    if (end_addr > kLow4GBoundary) {
      VirtualFree(allocated_ptr, 0, MEM_RELEASE);
      return nullptr;
    }

    Low4GAllocation alloc{};
    alloc.address = allocated_ptr;
    alloc.size = size;
    alloc.current_protection = initial_protect;
    allocations_.push_back(alloc);
  }

  return allocated_ptr;
}

bool JitLow4GAllocator::Protect(void* addr, size_t size, DWORD new_protect, DWORD* old_protect) {
  if (addr == nullptr || size == 0) {
    return false;
  }

  DWORD prev = 0;
  BOOL ok = VirtualProtect(addr, size, new_protect, &prev);
  if (!ok) {
    return false;
  }

  if (old_protect != nullptr) {
    *old_protect = prev;
  }

  // Update recorded protection
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& alloc : allocations_) {
    if (alloc.address == addr) {
      alloc.current_protection = new_protect;
      break;
    }
  }

  return true;
}

bool JitLow4GAllocator::MakeExecutable(void* addr, size_t size) {
  if (!Protect(addr, size, PAGE_EXECUTE_READ, nullptr)) {
    return false;
  }
  // Flush processor instruction cache to guarantee instruction pipeline coherence
  FlushInstructionCache(GetCurrentProcess(), addr, size);
  return true;
}

bool JitLow4GAllocator::MakeWritable(void* addr, size_t size) {
  return Protect(addr, size, PAGE_EXECUTE_READWRITE, nullptr);
}

bool JitLow4GAllocator::Free(void* addr, size_t size) {
  if (addr == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = allocations_.begin(); it != allocations_.end(); ++it) {
    if (it->address == addr) {
      VirtualFree(addr, 0, MEM_RELEASE);
      allocations_.erase(it);
      return true;
    }
  }

  (void)size;
  return VirtualFree(addr, 0, MEM_RELEASE) != 0;
}

void JitLow4GAllocator::FreeAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& alloc : allocations_) {
    if (alloc.address != nullptr) {
      VirtualFree(alloc.address, 0, MEM_RELEASE);
      alloc.address = nullptr;
    }
  }
  allocations_.clear();
}

size_t JitLow4GAllocator::GetAllocationCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return allocations_.size();
}

// ============================================================================
// 4. Initialization and Lifecycle Hooks
// ============================================================================

bool InitializeJitWindows(size_t initial_capacity, size_t max_capacity) {
  (void)initial_capacity;
  (void)max_capacity;
  // Ensure allocators and unwinding structures are primed
  JitLow4GAllocator::Instance();
  JitSehUnwindRegistry::Instance();
  return true;
}

void ShutdownJitWindows() {
  JitSehUnwindRegistry::Instance().UnregisterAll();
  JitLow4GAllocator::Instance().FreeAll();
}

}  // namespace windows
}  // namespace jit
}  // namespace art

// ============================================================================
// 5. C Linkage Interface Implementation
// ============================================================================

extern "C" {

void* art_jit_windows_alloc_low4g(size_t size) {
  return art::jit::windows::JitLow4GAllocator::Instance().Allocate(
      size, PAGE_EXECUTE_READWRITE);
}

bool art_jit_windows_free_low4g(void* ptr, size_t size) {
  return art::jit::windows::JitLow4GAllocator::Instance().Free(ptr, size);
}

bool art_jit_windows_protect(void* ptr, size_t size, DWORD new_protect, DWORD* old_protect) {
  return art::jit::windows::JitLow4GAllocator::Instance().Protect(
      ptr, size, new_protect, old_protect);
}

bool art_jit_windows_make_executable(void* ptr, size_t size) {
  return art::jit::windows::JitLow4GAllocator::Instance().MakeExecutable(ptr, size);
}

bool art_jit_windows_make_writable(void* ptr, size_t size) {
  return art::jit::windows::JitLow4GAllocator::Instance().MakeWritable(ptr, size);
}

bool art_jit_windows_register_unwind(void* base_addr, size_t size, const char* name) {
  return art::jit::windows::JitSehUnwindRegistry::Instance().RegisterCodeChunk(
      base_addr, size, name);
}

bool art_jit_windows_unregister_unwind(void* base_addr) {
  return art::jit::windows::JitSehUnwindRegistry::Instance().UnregisterCodeChunk(base_addr);
}

bool art_jit_windows_init() {
  return art::jit::windows::InitializeJitWindows();
}

void art_jit_windows_shutdown() {
  art::jit::windows::ShutdownJitWindows();
}

uint64_t art_jit_call_sysv_from_win64(void* fn, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
  return art::jit::windows::CallSysVFromWin64(fn, a1, a2, a3, a4, 0, 0);
}

uint64_t art_jit_call_win64_from_sysv(void* fn, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
  return art::jit::windows::CallWin64FromSysV(fn, a1, a2, a3, a4);
}

}  // extern "C"
