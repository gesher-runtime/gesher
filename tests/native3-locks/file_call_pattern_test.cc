// Portable instruction-pattern checks only: these do not claim Windows waits.
#include "file_call_pattern.h"
#include <array>
#include <cstdio>
#include <cstdlib>

using native3_stack_probe::FileArgumentRegister;
using native3_stack_probe::FileCallPattern;

int main() {
  unsigned checks = 0;
  auto check = [&](bool ok) {
    ++checks;
    if (!ok) {
      std::fprintf(stderr, "FILE call pattern check %u failed\n", checks);
      std::exit(1);
    }
  };
  constexpr uintptr_t stream = 0x12340, other = 0x56780;
  // Bytes at RVA 0x6187..0x6196 in the actual failed CI command-worker binary:
  // SHA256 2c4aef9f11340f328ad6acea6caf98fc140287a0ea4d592854008484e96bd6e7.
  constexpr std::array<unsigned char, 15> indirect_return = {
      0x48, 0x89, 0xd7, 0x48, 0x89, 0xce, 0x48, 0x89, 0xd1,
      0xff, 0x15, 0xba, 0x57, 0x0f, 0x00};
  check(FileCallPattern(indirect_return.data(), indirect_return.size(), other, stream, stream) == FileArgumentRegister::Rdi);
  check(FileCallPattern(indirect_return.data(), indirect_return.size(), stream, other, stream) == FileArgumentRegister::None);
  check(FileCallPattern(indirect_return.data(), indirect_return.size(), other, stream, other) == FileArgumentRegister::None);
  check(FileCallPattern(indirect_return.data(), indirect_return.size(), other, stream, 0) == FileArgumentRegister::None);
  check(FileCallPattern(nullptr, indirect_return.size(), other, stream, stream) == FileArgumentRegister::None);
  for (size_t size = 0; size < indirect_return.size(); ++size)
    check(FileCallPattern(indirect_return.data() + indirect_return.size() - size, size,
                         other, stream, stream) == FileArgumentRegister::None);
  for (size_t index = 0; index < 11; ++index) {
    auto bad = indirect_return;
    bad[index] ^= 1;
    check(FileCallPattern(bad.data(), bad.size(), other, stream, stream) == FileArgumentRegister::None);
  }
  for (unsigned char opcode : {0xce, 0xf1}) {
    std::array<unsigned char, 9> old = {0x48, 0x89, opcode, 0xff, 0x15, 0, 0, 0, 0};
    check(FileCallPattern(old.data(), old.size(), stream, other, stream) == FileArgumentRegister::Rsi);
    check(FileCallPattern(old.data(), old.size(), other, stream, stream) == FileArgumentRegister::None);
    for (size_t index = 0; index < 5; ++index) {
      auto bad = old;
      bad[index] ^= 1;
      check(FileCallPattern(bad.data(), bad.size(), stream, other, stream) == FileArgumentRegister::None);
    }
  }
  std::printf("FILE_CALL_PATTERNS checks=%u failures=0 scope=instruction-pattern-only\n", checks);
}
