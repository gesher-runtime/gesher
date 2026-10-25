// gesher arm64 translator: ABI marshalling thunks between the host (Microsoft
// x64, this DLL's own convention) and the host's System V entry points the
// guest calls. docs/decisions/0047-arm64-translation.md.
//
// The DLL is built with llvm-mingw clang, so its functions use the Microsoft
// x64 ABI. The host functions a guest import resolves to (the bionic shim, the
// JNI proxies, libandroid, EGL/GLES) are System V (ADR 0006/0017). These
// thunks move arguments between the two conventions explicitly, and save what
// one convention preserves and the other does not.

	.intel_syntax noprefix
	.text

// uint64_t gesher_call_host_sysv(HostCall* call);  // rcx = call
//
//   struct HostCall { void* fn;              // +0
//                     const uint64_t* gpr;   // +8:  X0..X5
//                     const uint64_t* xmm;   // +16: V0..V7 low 64 bits
//                     const uint64_t* stack; // +24: X6, X7, then guest stack args
//                     uint64_t nstack;       // +32: count of `stack`
//                     uint64_t out_xmm0;     // +40: written with the result's xmm0
//                     uint64_t out_rdx; };   // +48: written with the result's rdx
//
// Calls System V `fn` with X0..X5 in rdi,rsi,rdx,rcx,r8,r9 and V0..V7 in
// xmm0..xmm7, the `stack` words as System V stack arguments (System V's 7th and
// 8th arguments and any further ones), and AL = 8 for a variadic callee.
// Returns rax and stores xmm0/rdx in the HostCall.
//
// Microsoft x64 preserves rsi, rdi and xmm6..xmm15 across a call; System V
// does not, so they are saved here (dynarmic's generated code keeps guest
// state in xmm6..xmm15 across the callbacks that end up here).
	.globl gesher_call_host_sysv
	.p2align 4
gesher_call_host_sysv:
	push rbp
	mov  rbp, rsp
	push rbx
	push rsi
	push rdi
	sub  rsp, 168                    // xmm6..xmm15 (160) + 8: rsp 16-aligned
	movdqu xmmword ptr [rsp + 0], xmm6
	movdqu xmmword ptr [rsp + 16], xmm7
	movdqu xmmword ptr [rsp + 32], xmm8
	movdqu xmmword ptr [rsp + 48], xmm9
	movdqu xmmword ptr [rsp + 64], xmm10
	movdqu xmmword ptr [rsp + 80], xmm11
	movdqu xmmword ptr [rsp + 96], xmm12
	movdqu xmmword ptr [rsp + 112], xmm13
	movdqu xmmword ptr [rsp + 128], xmm14
	movdqu xmmword ptr [rsp + 144], xmm15
	mov  rbx, rcx                    // the HostCall*, kept across the call
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
	mov  qword ptr [rbx + 48], rdx   // out_rdx
	lea  rsp, [rbp - 192]            // the xmm save area (24 + 168 below rbp)
	movdqu xmm6, xmmword ptr [rsp + 0]
	movdqu xmm7, xmmword ptr [rsp + 16]
	movdqu xmm8, xmmword ptr [rsp + 32]
	movdqu xmm9, xmmword ptr [rsp + 48]
	movdqu xmm10, xmmword ptr [rsp + 64]
	movdqu xmm11, xmmword ptr [rsp + 80]
	movdqu xmm12, xmmword ptr [rsp + 96]
	movdqu xmm13, xmmword ptr [rsp + 112]
	movdqu xmm14, xmmword ptr [rsp + 128]
	movdqu xmm15, xmmword ptr [rsp + 144]
	add  rsp, 168
	pop  rdi
	pop  rsi
	pop  rbx
	pop  rbp
	ret

// void gesher_guest_entry_shim(): entered (System V) by a host_entry stub with
// the guest function pointer in r10, as if called directly by the host caller.
// Builds an EntryFrame and calls gesher_guest_entry_trampoline(fn, frame)
// with the Microsoft x64 ABI, then returns the guest result (rax, rdx, and
// xmm0 for a floating-point return) to the System V caller.
//
//   struct EntryFrame { uint64_t gpr[8];          // +0:   X0..X7
//                       uint64_t xmm[8];          // +64:  V0..V7
//                       const uint64_t* stack;    // +128: the caller's stack args 9..
//                       uint64_t out_rdx;         // +136
//                       uint64_t out_xmm0; };     // +144
//
// System V passes integer arguments 7 and 8 on the stack ([rbp+16], [rbp+24]
// after the push below); AAPCS64 passes them in X6, X7. Further stack
// arguments start at [rbp+32] and are copied to the guest stack.
	.globl gesher_guest_entry_shim
	.p2align 4
gesher_guest_entry_shim:
	push rbp
	mov  rbp, rsp
	sub  rsp, 160
	mov  qword ptr [rsp + 0], rdi
	mov  qword ptr [rsp + 8], rsi
	mov  qword ptr [rsp + 16], rdx
	mov  qword ptr [rsp + 24], rcx
	mov  qword ptr [rsp + 32], r8
	mov  qword ptr [rsp + 40], r9
	mov  rax, qword ptr [rbp + 16]
	mov  qword ptr [rsp + 48], rax
	mov  rax, qword ptr [rbp + 24]
	mov  qword ptr [rsp + 56], rax
	movsd qword ptr [rsp + 64], xmm0
	movsd qword ptr [rsp + 72], xmm1
	movsd qword ptr [rsp + 80], xmm2
	movsd qword ptr [rsp + 88], xmm3
	movsd qword ptr [rsp + 96], xmm4
	movsd qword ptr [rsp + 104], xmm5
	movsd qword ptr [rsp + 112], xmm6
	movsd qword ptr [rsp + 120], xmm7
	lea  rax, [rbp + 32]
	mov  qword ptr [rsp + 128], rax
	mov  qword ptr [rsp + 136], 0
	mov  qword ptr [rsp + 144], 0
	// Microsoft x64 call: rcx = fn, rdx = frame, 32-byte shadow space.
	mov  rcx, r10
	lea  rdx, [rsp + 0]
	sub  rsp, 32
	call gesher_guest_entry_trampoline
	add  rsp, 32
	mov  rdx, qword ptr [rsp + 136]
	movsd xmm0, qword ptr [rsp + 144]
	add  rsp, 160
	pop  rbp
	ret
