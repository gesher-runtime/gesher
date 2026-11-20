// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Native callers/targets for the exact production thunks.S. These are ordinary
// Microsoft and System V functions; no translator or guest execution is used.
.intel_syntax noprefix
.text

// RegisterState: rbp/rbx/rsi/rdi, ten full XMMs, rsp, return_pc (208 bytes).
.macro snapshot pointer, return_label
  mov [\pointer + 0], rbp
  mov [\pointer + 8], rbx
  mov [\pointer + 16], rsi
  mov [\pointer + 24], rdi
  movdqu [\pointer + 32], xmm6
  movdqu [\pointer + 48], xmm7
  movdqu [\pointer + 64], xmm8
  movdqu [\pointer + 80], xmm9
  movdqu [\pointer + 96], xmm10
  movdqu [\pointer + 112], xmm11
  movdqu [\pointer + 128], xmm12
  movdqu [\pointer + 144], xmm13
  movdqu [\pointer + 160], xmm14
  movdqu [\pointer + 176], xmm15
  mov [\pointer + 192], rsp
  lea r11, [rip + \return_label]
  mov [\pointer + 200], r11
.endm

.macro caller_prolog name
  .globl \name
  .p2align 4
  .seh_proc \name
\name:
  push rbp
  .seh_pushreg rbp
  push rbx
  .seh_pushreg rbx
  push rsi
  .seh_pushreg rsi
  push rdi
  .seh_pushreg rdi
  sub rsp, 232
  .seh_stackalloc 232
  movdqu [rsp + 64], xmm6
  .seh_savexmm xmm6, 64
  movdqu [rsp + 80], xmm7
  .seh_savexmm xmm7, 80
  movdqu [rsp + 96], xmm8
  .seh_savexmm xmm8, 96
  movdqu [rsp + 112], xmm9
  .seh_savexmm xmm9, 112
  movdqu [rsp + 128], xmm10
  .seh_savexmm xmm10, 128
  movdqu [rsp + 144], xmm11
  .seh_savexmm xmm11, 144
  movdqu [rsp + 160], xmm12
  .seh_savexmm xmm12, 160
  movdqu [rsp + 176], xmm13
  .seh_savexmm xmm13, 176
  movdqu [rsp + 192], xmm14
  .seh_savexmm xmm14, 192
  movdqu [rsp + 208], xmm15
  .seh_savexmm xmm15, 208
  .seh_endprologue
.endm

.macro seed_registers
  movabs rbp, 0x1122334455667788
  movabs rbx, 0x2233445566778899
  movabs rsi, 0x33445566778899aa
  movabs rdi, 0x445566778899aabb
  lea r11, [rip + unwind_seed_xmm]
  movdqu xmm6, [r11 + 0]
  movdqu xmm7, [r11 + 16]
  movdqu xmm8, [r11 + 32]
  movdqu xmm9, [r11 + 48]
  movdqu xmm10, [r11 + 64]
  movdqu xmm11, [r11 + 80]
  movdqu xmm12, [r11 + 96]
  movdqu xmm13, [r11 + 112]
  movdqu xmm14, [r11 + 128]
  movdqu xmm15, [r11 + 144]
.endm

.macro caller_epilog
  mov rax, [rsp + 224]
  movdqu xmm6, [rsp + 64]
  movdqu xmm7, [rsp + 80]
  movdqu xmm8, [rsp + 96]
  movdqu xmm9, [rsp + 112]
  movdqu xmm10, [rsp + 128]
  movdqu xmm11, [rsp + 144]
  movdqu xmm12, [rsp + 160]
  movdqu xmm13, [rsp + 176]
  movdqu xmm14, [rsp + 192]
  movdqu xmm15, [rsp + 208]
  add rsp, 232
  pop rdi
  pop rsi
  pop rbx
  pop rbp
  ret
  .seh_endproc
.endm

// uint64_t UnwindCallHost(HostCall*, RegisterState* before, RegisterState* after)
caller_prolog UnwindCallHost
  mov [rsp + 32], rcx
  mov [rsp + 40], rdx
  mov [rsp + 48], r8
  seed_registers
  mov r10, [rsp + 40]
  snapshot r10, .Lhost_return
  mov rcx, [rsp + 32]
  call gesher_call_host_sysv
.Lhost_return:
  mov [rsp + 224], rax
  mov r10, [rsp + 48]
  snapshot r10, .Lhost_return
  caller_epilog

// uint64_t UnwindCallGuest(RegisterState* before, RegisterState* after, Result*)
caller_prolog UnwindCallGuest
  mov [rsp + 32], r8
  mov [rsp + 40], rcx
  mov [rsp + 48], rdx
  seed_registers
  lea r11, [rip + unwind_guest_gpr]
  mov rdi, [r11 + 0]
  mov rsi, [r11 + 8]
  mov rdx, [r11 + 16]
  mov rcx, [r11 + 24]
  mov r8, [r11 + 32]
  mov r9, [r11 + 40]
  mov rax, [r11 + 48]
  mov [rsp + 0], rax
  mov rax, [r11 + 56]
  mov [rsp + 8], rax
  mov rax, [r11 + 64]
  mov [rsp + 16], rax
  lea r11, [rip + unwind_fp_args]
  movsd xmm0, [r11 + 0]
  movsd xmm1, [r11 + 8]
  movsd xmm2, [r11 + 16]
  movsd xmm3, [r11 + 24]
  movsd xmm4, [r11 + 32]
  movsd xmm5, [r11 + 40]
  movsd xmm6, [r11 + 48]
  movsd xmm7, [r11 + 56]
  mov r10, [rsp + 40]
  snapshot r10, .Lguest_return
  mov r10, [rip + unwind_guest_function]
  mov eax, 8
  call gesher_guest_entry_shim
.Lguest_return:
  mov [rsp + 224], rax
  mov r10, [rsp + 32]
  mov [r10 + 0], rax
  mov [r10 + 8], rdx
  movsd [r10 + 16], xmm0
  mov r10, [rsp + 48]
  snapshot r10, .Lguest_return
  caller_epilog

// System V target: snapshot all arguments, then legally clobber RSI/RDI and
// XMM6..15 before invoking the native Windows context-capture probe.
.globl UnwindSysvTarget
.p2align 4
.seh_proc UnwindSysvTarget
UnwindSysvTarget:
  push rbp
  .seh_pushreg rbp
  push rbx
  .seh_pushreg rbx
  sub rsp, 184
  .seh_stackalloc 184
  .seh_endprologue
  mov [rsp + 32], rdi
  mov [rsp + 40], rsi
  mov [rsp + 48], rdx
  mov [rsp + 56], rcx
  mov [rsp + 64], r8
  mov [rsp + 72], r9
  movsd [rsp + 80], xmm0
  movsd [rsp + 88], xmm1
  movsd [rsp + 96], xmm2
  movsd [rsp + 104], xmm3
  movsd [rsp + 112], xmm4
  movsd [rsp + 120], xmm5
  movsd [rsp + 128], xmm6
  movsd [rsp + 136], xmm7
  movzx eax, al
  mov [rsp + 160], rax
  lea rax, [rsp + 208]
  mov [rsp + 144], rax
  lea rax, [rsp + 200]
  mov [rsp + 152], rax
  movabs rbp, 0xa1a2a3a4a5a6a7a8
  movabs rbx, 0xb1b2b3b4b5b6b7b8
  movabs rsi, 0xc1c2c3c4c5c6c7c8
  movabs rdi, 0xd1d2d3d4d5d6d7d8
  pxor xmm6, xmm6
  pxor xmm7, xmm7
  pxor xmm8, xmm8
  pxor xmm9, xmm9
  pxor xmm10, xmm10
  pxor xmm11, xmm11
  pxor xmm12, xmm12
  pxor xmm13, xmm13
  pxor xmm14, xmm14
  pxor xmm15, xmm15
  lea rcx, [rsp + 32]
  call UnwindProbeHost
  movabs rax, 0x8192a3b4c5d6e7f8
  movabs rdx, 0x92a3b4c5d6e7f809
  movsd xmm0, [rip + unwind_result_fp]
  add rsp, 184
  pop rbx
  pop rbp
  ret
.seh_endproc

// A legal Microsoft native callee of the production guest-entry shim.
// Pass its actual entry RSP to the C++ probe as argument 3.
.globl gesher_guest_entry_trampoline
.p2align 4
.seh_proc gesher_guest_entry_trampoline
gesher_guest_entry_trampoline:
  sub rsp, 40
  .seh_stackalloc 40
  .seh_endprologue
  lea r8, [rsp + 40]
  call UnwindProbeGuest
  add rsp, 40
  ret
.seh_endproc
