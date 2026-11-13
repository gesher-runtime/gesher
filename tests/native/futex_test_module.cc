#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
// Deliberately link a separate copy of gesher_posix into this PE module.
extern "C" __declspec(dllexport) long futex_module_call(
    volatile int* a, int op, int n, const timespec* t, volatile int* b, int m) {
  return syscall(SYS_futex, a, op, n, t, b, m);
}
