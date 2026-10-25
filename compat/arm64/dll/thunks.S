// gesher arm64 translator: ABI marshalling thunks between the host (Microsoft
// x64, this DLL's own convention) and the host's System V entry points the
// guest calls. docs/decisions/0047-arm64-translation.md.
//
// The DLL is built with llvm-mingw clang, so its functions use the Microsoft
// x64 ABI. The host functions a guest import resolves to (the bionic shim, the
// JNI proxies, libandroid, EGL/GLES) are System V (ADR 0006/0017). These
// thunks move arguments between the two conventions explicitly.

	.intel_syntax noprefix
	.text

// uint64_t gesher_call_host_sysv(HostCall* call);  // rcx = call
//
//   struct HostCall { void* fn;            // +0
//                     const uint64_t* gpr; // +8:  X0..X5
//                     const uint64_t* xmm; // +16: V0..V7 low 64 bits
//                     const uint64_t* stack; // +24: X6, X7, then guest stack args
//                     uint64_t nstack;     // +32: count of `stack` (even)
//                     uint64_t out_xmm0; }; // +40: written with the result's xmm0
//
// Calls System V `fn` with X0..X5 in rdi,rsi,rdx,rcx,r8,r9 and V0..V7 in
// xmm0..xmm7, the `stack` words as System V stack arguments (System V's 7th and
// 8th arguments and any variadic overflow), and AL = 8 for a variadic callee.
// Returns rax and stores the result's xmm0 low 64 bits in call->out_xmm0.
// Microsoft x64 entry; rbx holds `call` across the call (it survives, being
// MS-ABI callee-saved and restored at the end).
	.globl gesher_call_host_sysv
	.p2align 4
gesher_call_host_sysv:
	push rbp
	mov  rbp, rsp
	push rbx
	push rsi            // preserve MS-ABI callee-saved regs we clobber as SysV args
	push rdi
	mov  rbx, rcx       // the HostCall*, kept across the call
	// Reserve and fill the host stack arguments (16-byte aligned at the call).
	mov  r10, qword ptr [rbx + 24]   // stack ptr
	mov  r11, qword ptr [rbx + 32]   // nstack
	lea  rax, [r11*8]
	add  rax, 15
	and  rax, -16                    // bytes rounded up to 16
	and  rsp, -16
	sub  rsp, rax
	xor  rcx, rcx
2:
	cmp  rcx, r11
	jge  3f
	mov  rax, qword ptr [r10 + rcx*8]
	mov  qword ptr [rsp + rcx*8], rax
	inc  rcx
	jmp  2b
3:
	// SSE argument registers from xmm[0..7].
	mov  r10, qword ptr [rbx + 16]
	movsd xmm0, qword ptr [r10 + 0]
	movsd xmm1, qword ptr [r10 + 8]
	movsd xmm2, qword ptr [r10 + 16]
	movsd xmm3, qword ptr [r10 + 24]
	movsd xmm4, qword ptr [r10 + 32]
	movsd xmm5, qword ptr [r10 + 40]
	movsd xmm6, qword ptr [r10 + 48]
	movsd xmm7, qword ptr [r10 + 56]
	// Integer argument registers X0..X5 from gpr[0..5].
	mov  r9, qword ptr [rbx + 8]     // gpr ptr
	mov  rdi, qword ptr [r9 + 0]
	mov  rsi, qword ptr [r9 + 8]
	mov  rdx, qword ptr [r9 + 16]
	mov  rcx, qword ptr [r9 + 24]
	mov  r8,  qword ptr [r9 + 32]
	mov  r9,  qword ptr [r9 + 40]    // X5 (overwrites gpr ptr last)
	mov  eax, 8                      // AL = vector regs for a variadic callee
	call qword ptr [rbx + 0]         // fn
	movsd qword ptr [rbx + 40], xmm0 // out_xmm0
	lea  rsp, [rbp - 24]             // undo the pushes (rbx, rsi, rdi)
	pop  rdi
	pop  rsi
	pop  rbx
	pop  rbp
	ret

// void gesher_guest_entry_shim(): entered (System V) by a host_entry stub with
// the guest function pointer in r10. Spills the System V argument registers and
// calls gesher_guest_entry_trampoline(fn, gpr[8], xmm[8], out_xmm0) with the
// Microsoft x64 ABI, then returns the guest result (rax, and xmm0 for a
// floating-point return) to the System V caller.
	.globl gesher_guest_entry_shim
	.p2align 4
gesher_guest_entry_shim:
	push rbp
	mov  rbp, rsp
	sub  rsp, 160
	// gpr[8] at [rsp+0]: X0..X5 = rdi,rsi,rdx,rcx,r8,r9; X6,X7 = 0.
	mov  qword ptr [rsp + 0], rdi
	mov  qword ptr [rsp + 8], rsi
	mov  qword ptr [rsp + 16], rdx
	mov  qword ptr [rsp + 24], rcx
	mov  qword ptr [rsp + 32], r8
	mov  qword ptr [rsp + 40], r9
	mov  qword ptr [rsp + 48], 0
	mov  qword ptr [rsp + 56], 0
	// xmm[8] at [rsp+64]: V0..V7 low 64 bits.
	movsd qword ptr [rsp + 64], xmm0
	movsd qword ptr [rsp + 72], xmm1
	movsd qword ptr [rsp + 80], xmm2
	movsd qword ptr [rsp + 88], xmm3
	movsd qword ptr [rsp + 96], xmm4
	movsd qword ptr [rsp + 104], xmm5
	movsd qword ptr [rsp + 112], xmm6
	movsd qword ptr [rsp + 120], xmm7
	mov  qword ptr [rsp + 128], 0   // out_xmm0
	// Microsoft x64 call: rcx=fn, rdx=gpr, r8=xmm, r9=out_xmm0, 32B shadow.
	mov  rcx, r10
	lea  rdx, [rsp + 0]
	lea  r8,  [rsp + 64]
	lea  r9,  [rsp + 128]
	sub  rsp, 32
	call gesher_guest_entry_trampoline
	add  rsp, 32
	movsd xmm0, qword ptr [rsp + 128]
	add  rsp, 160
	pop  rbp
	ret
