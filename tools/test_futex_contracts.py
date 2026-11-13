#!/usr/bin/env python3
"""Run real production futex queues/guest conversion over a bounded host Win32 transport model.

--linux-reference calls the real Linux kernel with the same public contract fixture.
Separate PE module ownership and actual Windows wait transport are Windows CI gates.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT / 'compat/posix/src/syscall.cc')
    p.add_argument('--shim-source', type=Path, default=ROOT / 'compat/bionic/shim.cc')
    p.add_argument('--cxx', default=os.environ.get('CXX','c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--linux-reference', action='store_true')
    p.add_argument('--group', choices=['validation','public','concurrency','guest'], action='append')
    a = p.parse_args()
    groups = a.group or ['public','guest','concurrency']
    with tempfile.TemporaryDirectory(prefix='gesher-futex-') as d:
        d = Path(d)
        if a.linux_reference:
            generated = '#include <sys/syscall.h>\n#include "futex_contract_test.inc"\nstatic long invoke(volatile int* a,int op,int n,const timespec* t,volatile int* b,int m) { return syscall(SYS_futex,a,op,n,t,b,m); }\nint main() { return futex_run(invoke,invoke,1); }\n'
        else:
            source = a.source.read_text()
            shim = a.shim_source.read_text()
            start = 'int64_t BIONIC_SYSV Syscall('
            end = '\nvoid ReportUnresolved('
            if shim.count(start) != 1 or shim.count(end) != 1: raise ValueError('ambiguous syscall source boundary')
            guest = shim[shim.index(start):shim.index(end,shim.index(start))]
            print('futex source SHA256',hashlib.sha256(source.encode()).hexdigest(),flush=True)
            print('guest syscall section SHA256',hashlib.sha256(guest.encode()).hexdigest(),flush=True)
            (d / 'windows.h').write_text('#include "futex_windows_host.h"\n')
            owner = 'reinterpret_cast<FARPROC>(&gesher_futex_owner_v1)' if 'gesher_futex_owner_v1(' in source else 'nullptr'
            generated = '''#define _GNU_SOURCE 1
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <cstdint>
#include "futex_windows_host.h"
#define syscall gesher_test_syscall
''' + source + '''
static FARPROC GetProcAddress(HMODULE, const char* name) {
  return std::strcmp(name,"gesher_futex_owner_v1") == 0 ? '''+owner+''' : nullptr;
}
#define BIONIC_SYSV
struct LinuxTimespec { int64_t tv_sec, tv_nsec; };
static constexpr int kENOSYS=ENOSYS, kEINVAL=EINVAL, kEAGAIN=EAGAIN, kETIMEDOUT=ETIMEDOUT;
static bool DispatchTimerfdSyscall(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t*) { return false; }
''' + guest + '''
#undef syscall
#include "futex_contract_test.inc"
static long host(volatile int* a,int op,int n,const timespec* t,volatile int* b,int m) {
  return gesher_test_syscall(SYS_futex,a,op,n,t,b,m);
}
static long guest(volatile int* a,int op,int n,const timespec* t,volatile int* b,int m) {
  LinuxTimespec ts{}; int cmd = op & FUTEX_CMD_MASK;
  int64_t argument = reinterpret_cast<int64_t>(t);
  if (t && (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET)) { ts = {t->tv_sec,t->tv_nsec}; argument = reinterpret_cast<int64_t>(&ts); }
  return Syscall(202,reinterpret_cast<int64_t>(a),op,n,argument,reinterpret_cast<int64_t>(b),m);
}
static void wide_guest_timeout() {
  volatile int word = 0;
  for (int64_t ns : {INT64_C(4294967296),-INT64_C(4294967296),INT64_MAX,INT64_MIN}) {
    LinuxTimespec t{0,ns}; errno = 0;
    futex_check("guest timeout validated before narrowing",Syscall(202,reinterpret_cast<int64_t>(&word),FUTEX_WAIT_PRIVATE,1,reinterpret_cast<int64_t>(&t),0,0) == -1 && errno == EINVAL);
  }
}
int main(int argc,char** argv) {
  if (argc != 2) return 2;
  if (!std::strcmp(argv[1],"validation")) { futex_wait_api=host; futex_wake_api=host; futex_inputs(); wide_guest_timeout(); }
  else if (!std::strcmp(argv[1],"public")) futex_run(host,host,0);
  else if (!std::strcmp(argv[1],"guest")) { wide_guest_timeout(); futex_run(guest,host,0); futex_run(host,guest,0); }
  else if (!std::strcmp(argv[1],"concurrency")) { injected_spurious=1; futex_wait_api=guest; futex_wake_api=host; futex_concurrency(); }
  else return 2;
  printf("futex group %s: %d checks, %d failures\\n",argv[1],futex_checks,futex_failures);
  return futex_failures != 0;
}
'''
        (d / 'test.cc').write_text(generated)
        cmd = shlex.split(a.cxx)+['-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
               '-fno-exceptions','-pthread','-I',str(d),'-I',str(ROOT/'tests/native'),str(d/'test.cc'),'-o',str(d/'test')]
        if a.sanitize: cmd += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True,timeout=120)
        if a.linux_reference: return subprocess.run([str(d/'test')],cwd=d,timeout=60).returncode
        failure = 0
        for group in groups:
            result = subprocess.run([str(d/'test'),group],cwd=d,timeout=90)
            failure |= result.returncode != 0
        return int(failure)
if __name__ == '__main__': raise SystemExit(main())
