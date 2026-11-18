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
# Independently reviewed bounded Poll scratch, with no descriptor/completion
# transport changes. Pin the published checkpoint and full bytes; additionally
# keep every byte outside the exact native-Poll region at the original epoch.
# tools/test_native_poll_allocations.py covers reentry, retirement, UDP retry,
# pending flags, wake sockets, bounded retention and thread-exit destruction.
WINSOCK_BASE='eb652ece90f327622d1971867ab1730ae71ea786'
WINSOCK_SHA256='190b659777f3df317e4dc90f9df322a95bd2c840ea00ad2976f9dbf56935b02d'
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
SYSTEM_SHA256='3e6fc1241b54dbe11a203b01e208f2dba372101e4a54ea96d839da89f6e1c2b0'
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
# The reviewed TCP fixture changes add select/thread diagnostics and two
# final call-site labels. Derive the complete expected file from BASE using
# only these unique literal replacements; no function region is excluded.
# All assertion predicates, deadlines and other fixture bytes remain exact.
TCP_DIAGNOSTIC_REPLACEMENTS=(
 (br'''static void ready(int fd, bool urgent, int timeout = 0) {
  pollfd p{fd, POLLPRI, 0};
  require(poll(&p, 1, timeout) == (urgent ? 1 : 0), urgent ? "poll urgent" : "poll no urgent");
  require(p.revents == (urgent ? POLLPRI : 0), "priority mask");
  fd_set exceptions; FD_ZERO(&exceptions); FD_SET(fd, &exceptions);
  timeval zero{};
  require(select(fd + 1, nullptr, nullptr, &exceptions, &zero) == (urgent ? 1 : 0), "select exception count");
  require((FD_ISSET(fd, &exceptions) != 0) == urgent, "select exception membership");
}
''',
  br'''static void ready(int fd, bool urgent, int timeout = 0,
                  const char* phase = "scenario", int call_line = __builtin_LINE()) {
  pollfd p{fd, POLLPRI, 0};
  const int polled = poll(&p, 1, timeout);
  require(polled == (urgent ? 1 : 0), urgent ? "poll urgent" : "poll no urgent");
  require(p.revents == (urgent ? POLLPRI : 0), "priority mask");
  const short poll_revents = p.revents;
  fd_set exceptions; FD_ZERO(&exceptions); FD_SET(fd, &exceptions);
  timeval zero{};
  const int errno_before = errno;
  const int selected = select(fd + 1, nullptr, nullptr, &exceptions, &zero);
  const int errno_after = errno;
  auto diagnose = [&](bool member) {
    fprintf(stderr, "TCP_PRIORITY_SELECT_DIAGNOSTIC phase=%s line=%d fd=%d expected_urgent=%d "
            "poll_timeout_ms=%d poll_rc=%d poll_revents=%d select_rc=%d "
            "errno_before=%d errno_after=%d member=%d\n",
            phase, call_line, fd, urgent, timeout, polled, poll_revents, selected,
            errno_before, errno_after, member);
    errno = errno_after;  // Diagnostic I/O must not replace the assertion's errno.
  };
  if (selected != (urgent ? 1 : 0)) diagnose(FD_ISSET(fd, &exceptions) != 0);
  require(selected == (urgent ? 1 : 0), "select exception count");
  const bool member = FD_ISSET(fd, &exceptions) != 0;
  if (member != urgent) diagnose(member);
  require(member == urgent, "select exception membership");
}
'''),
 (b'''  require(pthread_create(&thread, nullptr, delayed, &a.sender) == 0, "sender thread");
  ready(a.receiver, true, 2000);
''',
  b'''  require(pthread_create(&thread, nullptr, delayed, &a.sender) == 0, "sender thread");
  ready(a.receiver, true, 2000, "delayed-arrival");
'''),
 (b'''  char byte{}; require(recv(a.receiver, &byte, 1, MSG_OOB) == 1 && byte == '?', "concurrent byte preserved");
  ready(a.receiver, false, 30);
''',
  b'''  char byte{}; require(recv(a.receiver, &byte, 1, MSG_OOB) == 1 && byte == '?', "concurrent byte preserved");
  ready(a.receiver, false, 30, "delayed-consumed");
'''),
)
TCP_OWNER_DIAGNOSTIC_REPLACEMENTS=(
 (b'''namespace gesher::test { int NativeUdpErrorControl(bool blocking); void NativeSupplierRetirement(); void DumpOwnHandleTypes(int mode, uintptr_t caller = 0, uintptr_t observer = 0); }
''',
  b'''namespace gesher::test { int NativeUdpErrorControl(bool blocking); void NativeSupplierRetirement(); void DumpOwnHandleTypes(int mode, uintptr_t caller = 0, uintptr_t observer = 0); }
namespace gesher::test { void ObserveOwnerThreads(int mode, bool failed); }
'''),
 (b'''    require(actual.handles == baseline.handles && actual.threads == baseline.threads, "repeated real ownership returns to warm handle and thread baseline");
''',
  b'''    gesher::test::ObserveOwnerThreads(mode, actual.handles != baseline.handles || actual.threads != baseline.threads);
    require(actual.handles == baseline.handles && actual.threads == baseline.threads, "repeated real ownership returns to warm handle and thread baseline");
'''),
)
# A separate HOST-only observer records bounded post-census TIDs. It queries
# thread start modules only after a mismatch, then returns to the unchanged
# assertion. Every production POSIX byte, supplier and deadline stays pinned.
OWNER_THREAD_DIAGNOSTIC_PATH='tests/net-tcp-priority/owner_thread_diagnostics.h'
OWNER_THREAD_DIAGNOSTIC_SHA256='83bdda8c98bc35761d5d2df001990ec155dcabda6a571d75b5057f1f25653f1b'
OWNER_THREAD_DIAGNOSTIC_INCLUDE=b'#include "owner_thread_diagnostics.h"\n'
# Only the successful protocol fixture's startup budget changed. Pin its exact
# reviewed bytes separately; controller, worker and execution gates retain BASE.
CONTROL_BASE='c62c0ea4f88447c64608851ecf661f630e259442'
# Reviewed fork-PR diagnostics add only optional source identity observations.
# Keep the original diagnostics byte-for-byte after removing these exact
# additions, and pin the complete new file independently. Acceptance remains
# in the callers; the helper still does not accept, reject or retry inputs.
RECEIPT_DIAGNOSTICS_SHA256='6f9c75a7042b6777893cddfaf000498414ad192f2d006cd6a41d429a51d39b77'
RECEIPT_SOURCE_PARAMETER=b'[Collections.IDictionary] $Expected = $null, [object] $SourceIdentity = $null)'
RECEIPT_SOURCE_BLOCK=b'''  if ($null -ne $SourceIdentity) {
    $Expected['source'] = $SourceIdentity.api_head_sha
    $Expected['checkout'] = $SourceIdentity.checkout_sha
    $Expected['head_repository_id'] = $SourceIdentity.head_repository_id
  }
'''
def old(path):return subprocess.check_output(['git','-C',str(ROOT),'show',BASE+':'+path])
def check():
 assert hashlib.sha256((ROOT/OWNER_THREAD_DIAGNOSTIC_PATH).read_bytes()).hexdigest()==OWNER_THREAD_DIAGNOSTIC_SHA256,OWNER_THREAD_DIAGNOSTIC_PATH
 paths=subprocess.check_output(['git','-C',str(ROOT),'diff','1745e99d7f2849ba81784545e07d60cb2b7800c3^','1745e99d7f2849ba81784545e07d60cb2b7800c3','--name-only'],text=True).splitlines()
 paths+=['.github/workflows/net-tcp-coherent-component.yml','tests/net-tcp-priority/contract.cc','tests/net-tcp-priority/native_udp_control.cc','tests/native2/net_udp_trunc.cpp','tests/native2/net_udp_fault_observer.c','tools/net-tcp-priority/qualified/TsfJobSupervisor.cs','tools/windows4/TsfJobSupervisor.cs','tools/windows4/TsfJobWorker.cs','tools/windows4/test-tsf-job-supervisor.ps1','tools/ci/receipt-diagnostics.ps1','tools/net-tcp-priority/import-policy.json','tools/net-tcp-priority/import_policy.py','tools/net-tcp-priority/build-guests.py','tools/net-tcp-priority/prepare-ndk.py']
 for path in paths:
  epoch=(FD_BASE if path=='compat/posix/src/fd.cc' else
         SOCKET_BASE if path=='compat/posix/src/socket.cc' else
         WINSOCK_BASE if path=='compat/posix/src/winsock.cc' else
         CONTROL_BASE if path=='tools/windows4/test-tsf-job-supervisor.ps1' else BASE)
  expected=subprocess.check_output(['git','-C',str(ROOT),'show',epoch+':'+path])
  actual=(ROOT/path).read_bytes()
  if path=='compat/posix/src/winsock.cc':
   assert actual==expected and hashlib.sha256(actual).hexdigest()==WINSOCK_SHA256,path
   original=old(path)
   start=b"namespace {\n// Winsock's WSAPOLLFD"
   end=b'// --- names --------------------------------------------------------------------'
   assert expected.count(start)==original.count(start)==1 and expected.count(end)==original.count(end)==1,path
   assert expected[:expected.index(start)]==original[:original.index(start)],path
   assert expected[expected.index(end):]==original[original.index(end):],path
  elif path=='compat/posix/src/fd.cc':
   assert actual.count(FD_DEVICE_LOOKUP)==1 and FD_DEVICE_LOOKUP not in expected,path
   assert actual.replace(FD_DEVICE_LOOKUP,b'',1)==expected,path
   assert hashlib.sha256(actual).hexdigest()==FD_SHA256,path
  elif path=='tests/net-tcp-priority/contract.cc':
   for before,after in TCP_DIAGNOSTIC_REPLACEMENTS:
    assert expected.count(before)==actual.count(after)==1,path
    assert after not in expected and before not in actual,path
    expected=expected.replace(before,after,1)
   for before,after in TCP_OWNER_DIAGNOSTIC_REPLACEMENTS:
    assert expected.count(before)==actual.count(after)==1 and after not in expected,path
    expected=expected.replace(before,after,1)
   assert actual==expected,path
  elif path=='tests/net-tcp-priority/native_udp_control.cc':
   anchor=b'#include "../../compat/posix/src/socket_internal.h"\n'
   assert expected.count(anchor)==1 and actual.count(OWNER_THREAD_DIAGNOSTIC_INCLUDE)==1,path
   assert actual.replace(OWNER_THREAD_DIAGNOSTIC_INCLUDE,b'',1)==expected,path
  elif path=='compat/posix/src/system.cc':
   for before,after in FCNTL_REPLACEMENTS:
    assert expected.count(before)==actual.count(after)==1,path
    assert after not in expected and before not in actual,path
    expected=expected.replace(before,after,1)
   expected=expected.replace(b'#include <string>\n',b'#include <string>\n#include <limits>\n',1)
   # Non-TCP POSIX contracts may change only in these reviewed regions;
   # SYSTEM_SHA256 below still pins every byte, including file-advice validation.
   actual_cursor=expected_cursor=0
   for start,end in [(b'// --- statvfs ---',b'\n// --- mount ---'),
                     (b'int sched_getaffinity(',b'\nint unshare('),
                     (b'int posix_fadvise(',b'\nint posix_fallocate('),
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
  elif path=='tools/ci/receipt-diagnostics.ps1':
   assert actual.count(RECEIPT_SOURCE_PARAMETER)==actual.count(RECEIPT_SOURCE_BLOCK)==2,path
   original=actual.replace(RECEIPT_SOURCE_PARAMETER,b'[Collections.IDictionary] $Expected = $null)')
   assert original.replace(RECEIPT_SOURCE_BLOCK,b'')==expected,path
   assert hashlib.sha256(actual).hexdigest()==RECEIPT_DIAGNOSTICS_SHA256,path
  elif path=='tools/net-tcp-priority/import-policy.json':
   policy=json.loads(expected)
   for role,entry in policy.items():
    if role!='gesher_arm64.dll':
     entry['system']+=['api-ms-win-core-realtime-l1-1-1.dll','powrprof.dll']
     entry['source']+=['compat/posix/src/resume.cc','compat/posix/include/gesher/clocks.h']
   assert actual==(json.dumps(policy,indent=2)+'\n').encode(),path
  elif path=='tools/net-tcp-priority/import_policy.py':
   addition=b"    # The POSIX clock/resume backend uses these exact Windows system imports.\n    # They do not authorize a power/clock dependency in the standalone translator.\n    for role in POLICY:\n        clocks = ['api-ms-win-core-realtime-l1-1-1.dll', 'powrprof.dll']\n        assert allowed(role, clocks) == (role != 'gesher_arm64.dll')\n        assert not allowed(role, ['api-ms-win-power-private-l1-1-0.dll'])\n"
   anchor=b"    assert not allowed('foreign-role', ['kernel32.dll'])\n"
   assert expected.count(anchor)==1 and actual==expected.replace(anchor,addition+anchor),path
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
 print('TCP_SELECTED_POLICY_PASS original_product3 reviewed_nested_epoll_fd reviewed_device_lookup_only reviewed_sendmsg_control reviewed_statvfs_affinity_file_advice_allocation_writev_and_memfd_only reviewed_fcntl_argument_selection_only canonical_Recv originalworkflow original_fixture_assertions_deadlines=EXACT reviewed_select_and_owner_thread_diagnostics_only controller_execution_tail original_import_NDK_build_roles=EXACT new_two_supplier_DAG=BOUNDED source12_patch2_exact native_processes_started=0')
if __name__=='__main__':check()
