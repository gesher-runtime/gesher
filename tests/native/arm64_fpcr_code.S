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
.size arm64_fpcr_code, .-arm64_fpcr_code
