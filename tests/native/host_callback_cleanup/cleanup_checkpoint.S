// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Test-only native checkpoint. The forced-unwind stop may restore it only after
// every abandoned owned resource and nested owner has completed cleanup.
// Neither helper allocates a frame or calls another function.
.intel_syntax noprefix
.text
.globl CleanupSave
.p2align 4
CleanupSave:
  mov [rcx + 0], rbp
  mov [rcx + 8], rbx
  mov [rcx + 16], rsi
  mov [rcx + 24], rdi
  mov [rcx + 32], r12
  mov [rcx + 40], r13
  mov [rcx + 48], r14
  mov [rcx + 56], r15
  lea rax, [rsp + 8]
  mov [rcx + 64], rax
  mov rax, [rsp]
  mov [rcx + 72], rax
  movdqu [rcx + 80], xmm6
  movdqu [rcx + 96], xmm7
  movdqu [rcx + 112], xmm8
  movdqu [rcx + 128], xmm9
  movdqu [rcx + 144], xmm10
  movdqu [rcx + 160], xmm11
  movdqu [rcx + 176], xmm12
  movdqu [rcx + 192], xmm13
  movdqu [rcx + 208], xmm14
  movdqu [rcx + 224], xmm15
  xor eax, eax
  ret

.globl CleanupRestore
.p2align 4
CleanupRestore:
  mov r10, rcx
  mov rbp, [r10 + 0]
  mov rbx, [r10 + 8]
  mov rsi, [r10 + 16]
  mov rdi, [r10 + 24]
  mov r12, [r10 + 32]
  mov r13, [r10 + 40]
  mov r14, [r10 + 48]
  mov r15, [r10 + 56]
  movdqu xmm6, [r10 + 80]
  movdqu xmm7, [r10 + 96]
  movdqu xmm8, [r10 + 112]
  movdqu xmm9, [r10 + 128]
  movdqu xmm10, [r10 + 144]
  movdqu xmm11, [r10 + 160]
  movdqu xmm12, [r10 + 176]
  movdqu xmm13, [r10 + 192]
  movdqu xmm14, [r10 + 208]
  movdqu xmm15, [r10 + 224]
  mov rsp, [r10 + 64]
  mov r11, [r10 + 72]
  mov eax, 1
  // No CRT longjmp/RtlUnwindEx: cleanup has already happened.
  // Do not restore historical FP control/status.
  jmp r11

// Real System V caller -> exact production guest-entry shim. This leaf tail
// stub adds no frame; its host caller supplies nine valid integer arguments.
.globl CleanupGuestEntry
.p2align 4
CleanupGuestEntry:
  lea r10, [rip + CleanupGuestCallback]
  jmp gesher_guest_entry_shim
