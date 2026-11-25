#include <sys/syscall.h>
#include <gesher/clocks.h>
#define FUTEX_TEST_CLOCK_GETTIME gesher::AndroidClockGettime
#include "futex_contract_test.inc"
#include "futex_contention_benchmark.h"
static long invoke(volatile int* a,int op,int n,const timespec* t,volatile int* b,int m) {
  return syscall(SYS_futex,a,op,n,t,b,m);
}
int main(int argc, char** argv) {
  if (argc == 2 && !std::strcmp(argv[1],"--benchmark")) return futex_contention_benchmark(invoke);
  if (argc != 1) return 2;
  return futex_run(invoke,invoke,1);
}
