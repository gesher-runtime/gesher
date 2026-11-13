#include <gesher/bionic_shim.h>
#include <sys/syscall.h>
#include <windows.h>
#include <initializer_list>
#include "futex_contract_test.inc"
#if defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))
#else
#define SYSV
#endif
extern "C" __declspec(dllimport) long futex_module_call(
    volatile int*, int, int, const timespec*, volatile int*, int);
using GuestSyscall = int64_t(SYSV*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);
static GuestSyscall guest_syscall;
struct GuestTime { int64_t seconds, nanoseconds; };
static long host(volatile int* a,int op,int n,const timespec* t,volatile int* b,int m) {
  return syscall(SYS_futex,a,op,n,t,b,m);
}
static long guest(volatile int* a,int op,int n,const timespec* t,volatile int* b,int m) {
  GuestTime time{}; int command = op & FUTEX_CMD_MASK;
  int64_t argument = reinterpret_cast<int64_t>(t);
  if (t && (command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET)) {
    time = {t->tv_sec,t->tv_nsec}; argument = reinterpret_cast<int64_t>(&time);
  }
  return static_cast<long>(guest_syscall(202,reinterpret_cast<int64_t>(a),op,n,argument,reinterpret_cast<int64_t>(b),m));
}
int main() {
  gesher::bionic::InstallBionicShim();
  guest_syscall = reinterpret_cast<GuestSyscall>(gesher::bionic::LookupBionicSymbol("syscall"));
  if (!guest_syscall) { puts("FAIL missing bionic syscall"); return 1; }
  futex_check("main executable owns futex export", GetProcAddress(GetModuleHandleW(nullptr),"gesher_futex_owner_v1") != nullptr);
  volatile int word = 0;
  for (int64_t ns : {INT64_C(4294967296),-INT64_C(4294967296),INT64_MAX,INT64_MIN}) {
    GuestTime time{0,ns}; errno = 0;
    futex_check("real guest entry validates full-width timeout",guest_syscall(202,reinterpret_cast<int64_t>(&word),FUTEX_WAIT_PRIVATE,1,reinterpret_cast<int64_t>(&time),0,0) == -1 && errno == EINVAL);
  }
  FutexApi routes[][2] = {{host,guest},{guest,host},{host,futex_module_call},{futex_module_call,host},
                         {guest,futex_module_call},{futex_module_call,guest},{futex_module_call,futex_module_call}};
  for (auto& route : routes) futex_run(route[0],route[1],0);
  if (!futex_failures) puts("futex_bridge_test all passed");
  return futex_failures != 0;
}
