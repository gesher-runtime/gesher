// gesher-base: <sys/ucontext.h> with the Linux x86-64 layout, which ART's
// fault handlers and Android native code expect. Gesher fills it from a
// Windows CONTEXT when delivering a signal, and copies it back afterwards.
#pragma once

#include <signal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  REG_R8 = 0, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
  REG_RDI, REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP,
  REG_RIP, REG_EFL, REG_CSGSFS, REG_ERR, REG_TRAPNO, REG_OLDMASK, REG_CR2
};
#define NGREG 23

typedef long long greg_t;
typedef greg_t gregset_t[NGREG];

struct _libc_fpxreg {
  unsigned short significand[4];
  unsigned short exponent;
  unsigned short padding[3];
};

struct _libc_xmmreg {
  uint32_t element[4];
};

struct _libc_fpstate {
  uint16_t cwd;
  uint16_t swd;
  uint16_t ftw;
  uint16_t fop;
  uint64_t rip;
  uint64_t rdp;
  uint32_t mxcsr;
  uint32_t mxcr_mask;
  struct _libc_fpxreg _st[8];
  struct _libc_xmmreg _xmm[16];
  uint32_t padding[24];
};
typedef struct _libc_fpstate* fpregset_t;

typedef struct {
  gregset_t gregs;
  fpregset_t fpregs;
  unsigned long long __reserved1[8];
} mcontext_t;

typedef struct ucontext_t {
  unsigned long long uc_flags;
  struct ucontext_t* uc_link;
  stack_t uc_stack;
  mcontext_t uc_mcontext;
  sigset_t uc_sigmask;
  unsigned long long __sigmask_pad[15];  // Linux sigset_t is 128 bytes
  struct _libc_fpstate __fpregs_mem;
  unsigned long long __ssp[4];
} ucontext_t;

#ifdef __cplusplus
}
#endif
