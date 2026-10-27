// Bare AArch64 entry for the independent host/guest rounding test. Built by
// the NDK in CI; the .text bytes contain no relocations or host implementation.
.text
.global arm64_fpcr_code
.type arm64_fpcr_code, %function
arm64_fpcr_code:
  cmp x0, #1
  b.eq .Lwrite
  cmp x0, #2
  b.eq .Lhost
  cmp x0, #3
  b.eq .Ladd
  cmp x0, #4
  b.eq .Lstatus
  cmp x0, #5
  b.eq .Lwrite_status
  cmp x0, #6
  b.eq .Ldivide
  cmp x0, #7
  b.eq .Lmultiply
  cmp x0, #8
  b.eq .Lsaturate
  cmp x0, #9
  b.eq .Ladd32
  cmp x0, #10
  b.eq .Lsubtract32
  cmp x0, #11
  b.eq .Lmultiply32
  cmp x0, #12
  b.eq .Ldivide32
  cmp x0, #13
  b.eq .Lsubtract
  mrs x0, fpcr
  ret
.Lwrite:
  msr fpcr, x1
  mrs x0, fpcr
  ret
.Lhost:
  mov x16, x1
  mov x0, x2
  mov x1, x3
  br x16
.Ladd:
  fmov d0, x1
  fmov d1, x2
  fadd d0, d0, d1
  fmov x0, d0
  ret
.Lstatus:
  mrs x0, fpsr
  ret
.Lwrite_status:
  msr fpsr, x1
  mrs x0, fpsr
  ret
.Ldivide:
  fmov d0, x1
  fmov d1, x2
  fdiv d0, d0, d1
  fmov x0, d0
  ret
.Lmultiply:
  fmov d0, x1
  fmov d1, x2
  fmul d0, d0, d1
  fmov x0, d0
  ret
.Lsaturate:
  movi v0.16b, #127
  movi v1.16b, #1
  sqadd v0.16b, v0.16b, v1.16b
  mrs x0, fpsr
  ret
.Ladd32:
  fmov s0, w1
  fmov s1, w2
  fadd s0, s0, s1
  fmov w0, s0
  ret
.Lsubtract32:
  fmov s0, w1
  fmov s1, w2
  fsub s0, s0, s1
  fmov w0, s0
  ret
.Lmultiply32:
  fmov s0, w1
  fmov s1, w2
  fmul s0, s0, s1
  fmov w0, s0
  ret
.Ldivide32:
  fmov s0, w1
  fmov s1, w2
  fdiv s0, s0, s1
  fmov w0, s0
  ret
.Lsubtract:
  fmov d0, x1
  fmov d1, x2
  fsub d0, d0, d1
  fmov x0, d0
  ret
.size arm64_fpcr_code, .-arm64_fpcr_code
