// Exact x64 call-site forms used by the CI-only FILE lock stack witness.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace native3_stack_probe {
enum class FileArgumentRegister { None, Rsi, Rdi };

inline FileArgumentRegister FileCallPattern(const unsigned char* code, size_t size,
                                           uintptr_t rsi, uintptr_t rdi, uintptr_t stream) {
  if (!code || !stream || size < 9) return FileArgumentRegister::None;
  const unsigned char* tail = code + size - 9;
  // mov rsi,rcx or mov rcx,rsi; call qword ptr [rip+disp32].
  if (rsi == stream && tail[0] == 0x48 && tail[1] == 0x89 &&
      (tail[2] == 0xce || tail[2] == 0xf1) && tail[3] == 0xff && tail[4] == 0x15)
    return FileArgumentRegister::Rsi;
  // AcquireLocal returns a shared_ptr through a hidden RCX argument. Its FILE
  // arrives in RDX, is saved in nonvolatile RDI, then becomes _lock_file's RCX.
  // Require the entire observed chain; RDI equality alone is not evidence.
  constexpr unsigned char indirect_return[] = {
      0x48, 0x89, 0xd7, 0x48, 0x89, 0xce, 0x48, 0x89, 0xd1, 0xff, 0x15};
  if (size >= 15 && rdi == stream &&
      std::memcmp(code + size - 15, indirect_return, sizeof(indirect_return)) == 0)
    return FileArgumentRegister::Rdi;
  return FileArgumentRegister::None;
}
}  // namespace native3_stack_probe
