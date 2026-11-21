// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Independent guest caller: verify actual imported sigsetjmp/siglongjmp,
// callee-saved GPRs/D registers/SP/PC, and Android's FPCR/FPSR non-restoration.
.text
.p2align 2
.global Arm64SetjmpRegisters
.type Arm64SetjmpRegisters, %function
Arm64SetjmpRegisters:
  sub sp, sp, #480
  stp x18, x19, [sp, #0]
  stp x20, x21, [sp, #16]
  stp x22, x23, [sp, #32]
  stp x24, x25, [sp, #48]
  stp x26, x27, [sp, #64]
  stp x28, x29, [sp, #80]
  stp x30, xzr, [sp, #96]
  stp d8, d9, [sp, #112]
  stp d10, d11, [sp, #128]
  stp d12, d13, [sp, #144]
  stp d14, d15, [sp, #160]
  mrs x9, fpcr
  mrs x10, fpsr
  stp x9, x10, [sp, #176]
  mov x9, #0x1234
  str x9, [sp, #456]
  mov x9, sp
  str x9, [sp, #464]

  // x18 must remain a valid platform/SCS pointer on the Android oracle.
  .irp reg,19,20,21,22,23,24,25,26,27,28,29
    mov x\reg, #(100 + \reg)
  .endr
  .irp reg,8,9,10,11,12,13,14,15
    mov x9, #(200 + \reg)
    fmov d\reg, x9
  .endr
  add x0, sp, #192
  mov w1, #1
  bl sigsetjmp
  cbnz w0, .Lreturned

  .irp reg,19,20,21,22,23,24,25,26,27,28,29
    mov x\reg, #0
  .endr
  .irp reg,8,9,10,11,12,13,14,15
    fmov d\reg, xzr
  .endr
  ldp x9, x10, [sp, #176]
  eor x9, x9, #0x400000
  orr x10, x10, #0x10
  msr fpcr, x9
  msr fpsr, x10
  sub sp, sp, #64
  add x0, sp, #256
  mov w1, #17
  bl siglongjmp
  brk #0

.Lreturned:
  cmp w0, #17
  b.ne .Lfailed
  ldr x9, [sp, #464]
  mov x10, sp
  cmp x9, x10
  b.ne .Lfailed
  ldr x9, [sp, #456]
  mov x10, #0x1234
  cmp x9, x10
  b.ne .Lfailed
  .irp reg,19,20,21,22,23,24,25,26,27,28,29
    cmp x\reg, #(100 + \reg)
    b.ne .Lfailed
  .endr
  .irp reg,8,9,10,11,12,13,14,15
    fmov x9, d\reg
    cmp x9, #(200 + \reg)
    b.ne .Lfailed
  .endr
  ldp x9, x10, [sp, #176]
  eor x9, x9, #0x400000
  orr x10, x10, #0x10
  mrs x11, fpcr
  mrs x12, fpsr
  cmp x9, x11
  b.ne .Lfailed
  cmp x10, x12
  b.ne .Lfailed
  mov w0, #0
  b .Lrestore
.Lfailed:
  mov w0, #1
.Lrestore:
  ldp x9, x10, [sp, #176]
  msr fpcr, x9
  msr fpsr, x10
  ldp d8, d9, [sp, #112]
  ldp d10, d11, [sp, #128]
  ldp d12, d13, [sp, #144]
  ldp d14, d15, [sp, #160]
  ldp x18, x19, [sp, #0]
  ldp x20, x21, [sp, #16]
  ldp x22, x23, [sp, #32]
  ldp x24, x25, [sp, #48]
  ldp x26, x27, [sp, #64]
  ldp x28, x29, [sp, #80]
  ldr x30, [sp, #96]
  add sp, sp, #480
  ret
.size Arm64SetjmpRegisters, .-Arm64SetjmpRegisters
.section .note.GNU-stack,"",%progbits
