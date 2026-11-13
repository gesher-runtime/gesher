"""Pure selected-consumer source/DAG preservation checks; no native execution."""
from pathlib import Path
import ast,hashlib,json,subprocess
ROOT=Path(__file__).resolve().parents[2]
BASE='657fb27a63c8171d540d6890ad47aa5fba5d26b2'
# fd.cc gained reviewed nested-epoll readiness and waiter wakeups. Keep those
# exact bytes pinned independently from the original TCP snapshot.
FD_BASE='0aafccec6d7c2cfdd88d73e721a3e1ce523edf2b'
# Positioned I/O needs the existing logical-device registry. The only new fd
# bytes expose its existing lookup; retain all registry, epoll and TCP behavior
# byte-for-byte against FD_BASE, and pin the complete reviewed file as well.
FD_DEVICE_LOOKUP=b'bool DeviceFromFd(int fd, Device* device) { return LookupDevice(fd, device); }\n\n'
FD_SHA256='7e2fed5465160602f7f01f29c5d93c4d3d04ec6c9139eaf5e54e205317305d4d'
# socket.cc gained the reviewed sendmsg ancillary-data rejection. Pin the
# published checkpoint independently; all other socket bytes and supplier
# inputs remain exact. Use the published commit, not local recovery history.
SOCKET_BASE='d5ad9aa318583ae3c3cca2d062ee964680e99e32'
# The reviewed file repairs change only statvfs/fstatvfs volume queries,
# sched_getaffinity capacity validation, posix_fallocate, its limits header and
# memfd_create, the virtual-descriptor writev route and fcntl argument selection.
# Pin the complete reviewed file, then independently retain every byte outside
# the exact substitutions and regions below.
# TCP/socket gather and read/pread/pwrite-vector suppliers stay at BASE.
# Only the two exact fcntl preamble fragments below differ from BASE.
# writev routing is covered by tools/test_host_event_loop_contracts.py and the
# shared Windows host/guest event-loop fixture. Affinity behavior is covered by
# tools/test_ordinary_os_contracts.py; filesystem statistics by
# tools/test_statvfs_contracts.py, the Windows POSIX probe and both ELF fixtures.
SYSTEM_SHA256='2b374aa6bf1b9030f4a8ce810462033d36643e274ee3a62ed1005db0d6efc667'
# These exact fcntl preamble substitutions preserve the lock-pointer path
# and every other fcntl/socket byte. Getters consume no operand; only supported
# integer commands consume a promoted int. No function-sized exclusion is added.
FCNTL_REPLACEMENTS=(
 (b'''  // Win64 long is32bits. Consume Linux lock pointers BEFORE entering the
  // unchanged integer-command varargs path.
''',
  b'''  // Win64 integer operands are 32 bits; lock operands retain pointer width.
  // Consume lock pointers before the integer-command varargs path.
'''),
 (b'''  va_list args;
  va_start(args, cmd);
  long arg = va_arg(args, long);
  va_end(args);
''',
  b'''  int arg = 0;
  // Getter commands have no third argument; other supported commands take an int.
  if (cmd == F_SETFD || cmd == F_SETFL || cmd == F_DUPFD ||
      cmd == F_DUPFD_CLOEXEC || cmd == F_ADD_SEALS) {
    va_list args;
    va_start(args, cmd);
    arg = va_arg(args, int);
    va_end(args);
  }
'''),
)
# Only the successful protocol fixture's startup budget changed. Pin its exact
# reviewed bytes separately; controller, worker and execution gates retain BASE.
CONTROL_BASE='c62c0ea4f88447c64608851ecf661f630e259442'
def old(path):return subprocess.check_output(['git','-C',str(ROOT),'show',BASE+':'+path])
def check():
 paths=subprocess.check_output(['git','-C',str(ROOT),'diff','1745e99d7f2849ba81784545e07d60cb2b7800c3^','1745e99d7f2849ba81784545e07d60cb2b7800c3','--name-only'],text=True).splitlines()
 paths+=['.github/workflows/net-tcp-coherent-component.yml','tests/net-tcp-priority/contract.cc','tests/net-tcp-priority/native_udp_control.cc','tests/native2/net_udp_trunc.cpp','tests/native2/net_udp_fault_observer.c','tools/net-tcp-priority/qualified/TsfJobSupervisor.cs','tools/windows4/TsfJobSupervisor.cs','tools/windows4/TsfJobWorker.cs','tools/windows4/test-tsf-job-supervisor.ps1','tools/ci/receipt-diagnostics.ps1','tools/net-tcp-priority/import-policy.json','tools/net-tcp-priority/import_policy.py','tools/net-tcp-priority/build-guests.py','tools/net-tcp-priority/prepare-ndk.py']
 for path in paths:
  epoch=(FD_BASE if path=='compat/posix/src/fd.cc' else
         SOCKET_BASE if path=='compat/posix/src/socket.cc' else
         CONTROL_BASE if path=='tools/windows4/test-tsf-job-supervisor.ps1' else BASE)
  expected=subprocess.check_output(['git','-C',str(ROOT),'show',epoch+':'+path])
  actual=(ROOT/path).read_bytes()
  if path=='compat/posix/src/fd.cc':
   assert actual.count(FD_DEVICE_LOOKUP)==1 and FD_DEVICE_LOOKUP not in expected,path
   assert actual.replace(FD_DEVICE_LOOKUP,b'',1)==expected,path
   assert hashlib.sha256(actual).hexdigest()==FD_SHA256,path
  elif path=='compat/posix/src/system.cc':
   for before,after in FCNTL_REPLACEMENTS:
    assert expected.count(before)==actual.count(after)==1,path
    assert after not in expected and before not in actual,path
    expected=expected.replace(before,after,1)
   expected=expected.replace(b'#include <string>\n',b'#include <string>\n#include <limits>\n',1)
   actual_cursor=expected_cursor=0
   for start,end in [(b'// --- statvfs ---',b'\n// --- mount ---'),
                     (b'int sched_getaffinity(',b'\nint unshare('),
                     (b'int posix_fallocate(',b'\nint fallocate('),
                     (b'ssize_t writev(',b'\nssize_t preadv('),
                     (b'int memfd_create(',b'\n// --- strsignal,')]:
    assert expected.count(start)==actual.count(start)==1 and expected.count(end)==actual.count(end)==1,path
    actual_start=actual.index(start);expected_start=expected.index(start)
    assert actual_start>=actual_cursor and expected_start>=expected_cursor,path
    assert actual[actual_cursor:actual_start]==expected[expected_cursor:expected_start],path
    actual_cursor=actual.index(end,actual_start);expected_cursor=expected.index(end,expected_start)
   assert actual[actual_cursor:]==expected[expected_cursor:],path
   assert hashlib.sha256(actual).hexdigest()==SYSTEM_SHA256,path
  else:assert actual==expected,path
 before=old('tools/net-tcp-priority/run-component.ps1').decode();after=(ROOT/'tools/net-tcp-priority/run-component.ps1').read_text(encoding='utf-8')
 marker='function RequireDiagnosticRetirement';assert after[after.index(marker):]==before[before.index(marker):],'Actual controller/guarded-copy/census/cancel/HOST/bothABI execution gates changed'
 raw=old('.github/workflows/net-tcp-coherent-component.yml').decode()
 expected=raw.replace('name: net-tcp-coherent-component','name: net-tcp-selected-component',1).replace('wip/tcp-coherent-port','wip/tcp-minimum-selection').replace('.github/workflows/net-tcp-coherent-component.yml','.github/workflows/net-tcp-selected-component.yml').replace('net-tcp-coherent-component-${{ github.ref }}','net-tcp-selected-component-${{ github.ref }}').replace('supplier: [canonical, retirement]','supplier: [canonical, selected-minimum]').replace('else prefix=tcp-private-supplier','else prefix=tcp-selected-supplier').replace('separately patched diagnostic compiler','intentionally selected two-patch original-profile candidate').replace('# canonical supplier separate from the minimal-retirement diagnostic supplier.','# canonical supplier separate from the intentionally selected minimum candidate.')
 expected=expected.replace('      - uses: actions/checkout@v4\n      - uses: ./.github/actions/setup-build','      - uses: actions/checkout@v4\n        with:\n          fetch-depth: 0\n      - name: Verify exact source and two-supplier preservation before native compilation\n        run: |\n          python3 tools/net-tcp-priority/test-selected-policy.py\n          python3 tools/net-tcp-priority/test-selected-supplier.py\n          python3 tools/host-pthread-authority/minimum_receipt.py selftest\n      - uses: ./.github/actions/setup-build',1)
 actual=(ROOT/'.github/workflows/net-tcp-selected-component.yml').read_text(encoding='utf-8');assert actual==expected,'Supplier-independent DAG/commands changed'
 pin=json.loads((ROOT/'tools/host-pthread-authority/minimum-inputs.json').read_text());assert pin['role']=='gesher-winpthreads-selected-minimum-v1' and len(pin['source_sha256'])==len(pin['output_source_sha256'])==12
 assert pin['ordered_patch_sha256']=={'minimum-detached-retirement.patch':'ebc5aac990c6ec075431d0c25a9b74ecadc0940da1bdad9503f358d4fe3431ad','minimum-noattach-join.patch':'c4f47903af720542a584fa743932316d86c45683da70a4c8620d7a71d3eb7f96'}
 for name,digest in pin['ordered_patch_sha256'].items():assert hashlib.sha256((ROOT/'tools/host-pthread-authority'/name).read_bytes()).hexdigest()==digest
 for path in ['tools/net-tcp-priority/manifest.py','tools/host-pthread-authority/minimum_receipt.py']:ast.parse((ROOT/path).read_text(encoding='utf-8'))
 print('TCP_SELECTED_POLICY_PASS original_product3 reviewed_nested_epoll_fd reviewed_device_lookup_only reviewed_sendmsg_control reviewed_statvfs_affinity_file_allocation_writev_and_memfd_only reviewed_fcntl_argument_selection_only canonical_Recv originalworkflow originalfixtures controller_execution_tail original_import_NDK_build_roles=EXACT new_two_supplier_DAG=BOUNDED source12_patch2_exact native_processes_started=0')
if __name__=='__main__':check()
