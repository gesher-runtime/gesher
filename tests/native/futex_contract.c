#define _GNU_SOURCE 1
#include <sys/syscall.h>
#include "futex_contract_test.inc"
static long invoke(volatile int* a,int op,int n,const struct timespec* t,volatile int* b,int m) {
  return syscall(SYS_futex,a,op,n,t,b,m);
}
__attribute__((visibility("default"))) int gesher_selftest(void) { return futex_run(invoke,invoke,1); }
