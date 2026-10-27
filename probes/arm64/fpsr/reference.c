// Freestanding NDK AArch64 instructions. CI runs this only in pinned QEMU,
// whose independent ARM/SoftFloat semantics supply raw numeric/status results.
#include <stddef.h>
#include <stdint.h>
#include "vectors.h"

static __attribute__((noreturn)) void Finish(unsigned status) {
  register uint64_t x0 __asm__("x0") = status;
  register uint64_t x8 __asm__("x8") = 93;
  __asm__ volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
  __builtin_unreachable();
}
static void Write(const char* bytes, size_t size) {
  register uint64_t x0 __asm__("x0") = 1;
  register const char* x1 __asm__("x1") = bytes;
  register size_t x2 __asm__("x2") = size;
  register uint64_t x8 __asm__("x8") = 64;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  if (x0 != size) Finish(2);
}
static char* Hex(char* output, uint64_t value) {
  static const char digits[] = "0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4) *output++ = digits[(value >> shift) & 15];
  *output++ = ' ';
  return output;
}
static uint64_t Arithmetic(unsigned operation, unsigned width, uint64_t left, uint64_t right) {
  uint64_t result;
  if (width == 32) {
    uint32_t output, a = (uint32_t)left, b = (uint32_t)right;
    switch (operation) {
      case ARM_FPSR_ADD:
        __asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fadd s0, s0, s1\n fmov %w0, s0"
                         : "=r"(output) : "r"(a), "r"(b) : "v0", "v1"); break;
      case ARM_FPSR_SUB:
        __asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fsub s0, s0, s1\n fmov %w0, s0"
                         : "=r"(output) : "r"(a), "r"(b) : "v0", "v1"); break;
      case ARM_FPSR_MUL:
        __asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fmul s0, s0, s1\n fmov %w0, s0"
                         : "=r"(output) : "r"(a), "r"(b) : "v0", "v1"); break;
      default:
        __asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fdiv s0, s0, s1\n fmov %w0, s0"
                         : "=r"(output) : "r"(a), "r"(b) : "v0", "v1"); break;
    }
    return output;
  }
  switch (operation) {
    case ARM_FPSR_ADD:
      __asm__ volatile("fmov d0, %1\n fmov d1, %2\n fadd d0, d0, d1\n fmov %0, d0"
                       : "=r"(result) : "r"(left), "r"(right) : "v0", "v1"); break;
    case ARM_FPSR_SUB:
      __asm__ volatile("fmov d0, %1\n fmov d1, %2\n fsub d0, d0, d1\n fmov %0, d0"
                       : "=r"(result) : "r"(left), "r"(right) : "v0", "v1"); break;
    case ARM_FPSR_MUL:
      __asm__ volatile("fmov d0, %1\n fmov d1, %2\n fmul d0, d0, d1\n fmov %0, d0"
                       : "=r"(result) : "r"(left), "r"(right) : "v0", "v1"); break;
    default:
      __asm__ volatile("fmov d0, %1\n fmov d1, %2\n fdiv d0, d0, d1\n fmov %0, d0"
                       : "=r"(result) : "r"(left), "r"(right) : "v0", "v1"); break;
  }
  return result;
}

__attribute__((noreturn)) void _start(void) {
  static const char header[] = "ARM64_FPSR_QEMU_V1\n";
  Write(header, sizeof(header) - 1);
  for (unsigned mode = 0; mode < 4; ++mode) for (unsigned dn = 0; dn < 2; ++dn) for (unsigned fz = 0; fz < 2; ++fz) {
    uint64_t control = ((uint64_t)mode << 22) | ((uint64_t)dn << 25) | ((uint64_t)fz << 24);
    for (unsigned width = 32; width <= 64; width += 32) for (unsigned operation = 0; operation < 4; ++operation) {
      for (unsigned index = 0; index < ARM_FPSR_VECTOR_COUNT; ++index) for (unsigned initial = 0; initial < 2; ++initial) {
        const struct ArmFpsrVector* vector = &ArmFpsrVectors[index];
        uint64_t left = width == 32 ? vector->left32 : vector->left64;
        uint64_t right = width == 32 ? vector->right32 : vector->right64;
        uint64_t seed = initial ? UINT64_C(0x08000081) : 0;
        __asm__ volatile("msr fpcr, %0\n msr fpsr, %1" : : "r"(control), "r"(seed) : "memory");
        uint64_t result = Arithmetic(operation, width, left, right);
        uint64_t status;
        __asm__ volatile("mrs %0, fpsr" : "=r"(status) : : "memory");
        char line[160];
        char* out = Hex(line, operation);
        out = Hex(out, width); out = Hex(out, control); out = Hex(out, left); out = Hex(out, right);
        out = Hex(out, seed); out = Hex(out, result); out = Hex(out, status);
        out[-1] = '\n';
        Write(line, (size_t)(out - line));
      }
    }
  }
  Finish(0);
}
