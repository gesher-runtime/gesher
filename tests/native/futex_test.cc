#include <sys/syscall.h>
#include "futex_contract_test.inc"
static long invoke(volatile int* a,int op,int n,const timespec* t,volatile int* b,int m) {
  return syscall(SYS_futex,a,op,n,t,b,m);
}
int main() { return futex_run(invoke,invoke,1); }
