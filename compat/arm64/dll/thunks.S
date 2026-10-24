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

// uint64_t gesher_call_host_sysv(void* fn,            // rcx
//                                const uint64_t* gpr, // rdx: X0..X7
//                                const uint64_t* xmm, // r8 : V0..V7 low 64 bits
//                                uint64_t* out_xmm0); // r9 : V0 of the result
//
// Calls System V `fn` with X0..X5 in rdi,rsi,rdx,rcx,r8,r9, X6,X7 as the first
// two stack arguments, and V0..V7 in xmm0..xmm7. Returns rax; stores the
// low 64 bits of the result's xmm0 through out_xmm0. Microsoft x64 entry.
	.globl gesher_call_host_sysv
	.p2align 4
gesher_call_host_sysv:
	push rbp
	mov  rbp, rsp
	push rbx
	push rsi            // preserve MS-ABI callee-saved regs we clobber as SysV args
	push rdi
	push r12
	mov  r12, rcx       // fn
	mov  r11, r9        // out_xmm0
	// Load the 8 SSE argument registers from xmm[0..7].
	movsd xmm0, qword ptr [r8 + 0]
	movsd xmm1, qword ptr [r8 + 8]
	movsd xmm2, qword ptr [r8 + 16]
	movsd xmm3, qword ptr [r8 + 24]
	movsd xmm4, qword ptr [r8 + 32]
	movsd xmm5, qword ptr [r8 + 40]
	movsd xmm6, qword ptr [r8 + 48]
	movsd xmm7, qword ptr [r8 + 56]
	// Stack arguments X6, X7, with 16-byte alignment at the call.
	mov  rax, qword ptr [rdx + 56]   // X7
	mov  r10, qword ptr [rdx + 48]   // X6
	sub  rsp, 32                     // 2 args (16) + 16 pad keeps rsp%16==0
	mov  qword ptr [rsp + 0], r10    // X6 -> first stack slot
	mov  qword ptr [rsp + 8], rax    // X7 -> second stack slot
	// Integer argument registers X0..X5.
	mov  rdi, qword ptr [rdx + 0]
	mov  rsi, qword ptr [rdx + 8]
	mov  r8,  qword ptr [rdx + 32]   // X4 (before clobbering rdx/rcx)
	mov  r9,  qword ptr [rdx + 40]   // X5
	mov  rcx, qword ptr [rdx + 24]   // X3
	mov  rax, qword ptr [rdx + 16]   // X2 via rax, then move
	mov  rdx, rax
	// rax = number of vector registers used (varargs AL); 8 is always safe.
	mov  eax, 8
	call r12
	// Return: rax already holds the integer result; capture xmm0.
	test r11, r11
	je   1f
	movsd qword ptr [r11], xmm0
1:
	add  rsp, 32
	pop  r12
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
