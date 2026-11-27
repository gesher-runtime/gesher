"""Pure selected-consumer source/DAG preservation checks; no native execution."""
from pathlib import Path
import ast,hashlib,json,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]

def live_source(path):
 return (ROOT/path).read_bytes()
BASE='657fb27a63c8171d540d6890ad47aa5fba5d26b2'
# fd.cc gained reviewed nested-epoll readiness and waiter wakeups. Keep those
# exact bytes pinned independently from the original TCP snapshot.
FD_BASE='0aafccec6d7c2cfdd88d73e721a3e1ce523edf2b'
# Positioned I/O exposes the existing logical-device lookup. Stream close and
# reopen additionally retire device metadata before descriptor reuse. Pin both
# exact additions at their original namespace boundary; retain every other
# registry, epoll and TCP byte against FD_BASE except the poll storage changes below.
# tools/test_stream_device_lifetime.py covers production-adapter cleanup,
# descriptor reuse, aliases, failed closes/reopens and errno preservation.
FD_DEVICE_LOOKUP=b'bool DeviceFromFd(int fd, Device* device) { return LookupDevice(fd, device); }\n\n'
FD_STREAM_DEVICE_RETIREMENT=b'''void ForgetStreamDevice(int fd, bool closed_or_replaced) {
  Device device;
  if (!LookupDevice(fd, &device)) return;
  int saved = errno;
  if (closed_or_replaced || HandleFromFd(fd) == INVALID_HANDLE_VALUE) ForgetOfd(fd);
  errno = saved;
}

'''
# Reversing the exact OFD transition restores main's prior separate-device cleanup.
FD_PRECEDING_STREAM_DEVICE_RETIREMENT=FD_STREAM_DEVICE_RETIREMENT.replace(b'ForgetOfd(fd);', b'ForgetDevice(fd);')
FD_DEVICE_INSERTION=b'}  // namespace gesher\n\nusing gesher::Device;\n'
# Outer poll reuses bounded working storage. Pin the helper as well as the
# complete fd.cc and allow only the exact storage substitutions at the call
# site. The event-loop fixture covers reentry, references and capacity release.
FD_SHA256='2467a04d74956dbac19e851434cb19450a315f638cbc2e28f015799de5ad8d9c'
FD_POLL_SCRATCH_HEADER='compat/posix/src/poll_scratch.h'
FD_POLL_SCRATCH_SHA256='669038c41eb20231d58c371f1ad38ff2e340525a938c476635481d68e7e0d393'
FD_POLL_SCRATCH_REPLACEMENTS=(
 (b'#include "internal.h"\n#include "record_lock_descriptors.h"\n',
  b'#include "internal.h"\n#include "poll_scratch.h"\n#include "record_lock_descriptors.h"\n'),
 (b'''constexpr short kOut = POLLOUT | POLLWRNORM;

// Readiness of a non-socket fd:''',
  b'''constexpr short kOut = POLLOUT | POLLWRNORM;

struct FdPollScratch {
  std::vector<gesher::ws::PollEntry> sockets;
  std::vector<nfds_t> socket_index;
  std::vector<std::shared_ptr<gesher::EventFd>> eventfds;
  std::vector<nfds_t> eventfd_index;
  std::vector<HANDLE> handles;

  void Reset() {
    gesher::ClearPollScratch(handles);
    gesher::ClearPollScratch(eventfd_index);
    gesher::ClearPollScratch(eventfds);
    gesher::ClearPollScratch(socket_index);
    gesher::ClearPollScratch(sockets);
  }
};

// Readiness of a non-socket fd:'''),
 (b'''  std::vector<ws::PollEntry> sockets;
  std::vector<nfds_t> socket_index;
  // Held so that their events stay open while waited on.
  std::vector<std::shared_ptr<EventFd>> eventfds;
  std::vector<nfds_t> eventfd_index;
''',
  b'''  PollScratchLease<FdPollScratch> lease(nfds <= kPollScratchLimit);
  auto& scratch = lease.get();
  auto& sockets = scratch.sockets;
  auto& socket_index = scratch.socket_index;
  // Held so that their events stay open while waited on.
  auto& eventfds = scratch.eventfds;
  auto& eventfd_index = scratch.eventfd_index;
'''),
 (b'''  bool look_first = waiter != nullptr;
  std::vector<HANDLE> handles;
''',
  b'''  bool look_first = waiter != nullptr;
  auto& handles = scratch.handles;
'''),
)
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
# One zero-time native sample confirms a waited OOB notification before it
# becomes Linux POLLPRI. Undo only this exact reviewed replacement before the
# unchanged checkpoint, hunk and full-byte validations below. The original
# shared TCP contract, including its immediate select before sender join, stays
# byte-identical. The native Poll fixture covers early wake, timeout, error,
# mixed readiness and real delayed data at the production boundary.
TCP_PRIORITY_WAKE_BEFORE=b'''      if (result != SOCKET_ERROR) break;
'''
TCP_PRIORITY_WAKE_AFTER=b'''      if (result != SOCKET_ERROR) {
        // A waited Winsock OOB notification can precede the readable byte.
        // Confirm the current level once without waiting before publishing
        // POLLPRI. The outer poll keeps its original deadline if it is absent.
        bool confirm_priority = false;
        if (sample_wait != 0 && result > 0) {
          for (const auto& fd : active)
            confirm_priority |= (fd.revents & fd.events & POLLRDBAND) != 0;
        }
        if (confirm_priority) { sample_wait = 0; continue; }
        break;
      }
'''
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
# Pin reviewed select diagnostics and the calibrated Windows owner census.
# Derive the complete expected fixture from BASE using only these exact
# replacements. Every other assertion, deadline and byte stays pinned.
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
namespace gesher::test {
void ObserveOwnerThreads(int mode, bool failed);
bool CalibrateOwnerThreads();
bool QualifiedOwnerThreadsMatch(int mode);
}
'''),
 (b'''static void owned_wait_controls() {
''',
  b'''static void owned_wait_controls() {
  require(gesher::test::CalibrateOwnerThreads(), "classify the actual Windows pool and reject a retained ordinary thread");
'''),
 (b'''    gesher::ws::OwnerCensus actual{}; require(gesher::ws::ReadConnectOwnerCensus(&actual), "actual production owner handle/thread census");
''',
  b'''    require(retired_operation.expired() && retired_description.expired(), "all product owner references retire after last close");
    gesher::ws::OwnerCensus actual{}; require(gesher::ws::ReadConnectOwnerCensus(&actual), "actual production owner handle/thread census");
'''),
 (b'''    require(actual.handles == baseline.handles && actual.threads == baseline.threads, "repeated real ownership returns to warm handle and thread baseline");
''',
  b'''    const bool owners_match = gesher::test::QualifiedOwnerThreadsMatch(mode);
    gesher::test::ObserveOwnerThreads(mode, actual.handles != baseline.handles || !owners_match);
    require(actual.handles == baseline.handles && owners_match, "repeated real ownership returns to warm handles and exact non-pool thread identities");
'''),
)
# The HOST-only observer calibrates the exact system entry address and pins
# product thread identities. Every production POSIX byte stays pinned.
OWNER_THREAD_DIAGNOSTIC_PATH='tests/net-tcp-priority/owner_thread_diagnostics.h'
OWNER_THREAD_DIAGNOSTIC_SHA256='dfa3b44ce619a6d1eb053bc43ac978f30172ff73ddc30342f6fe75a5ba6e63a3'
OWNER_THREAD_DIAGNOSTIC_INCLUDE=b'#include "owner_thread_diagnostics.h"\n'
OWNER_THREAD_CONTROLLER_BEFORE=r'''  if ($Context -eq 'host-posix') {
    foreach ($mode in 0..3) {
      if ($text -notmatch "(?m)^TCP_PRIORITY_OWNED_WAIT mode=$mode .*native_retired=1\r?$") { throw "Missing HOST actual mode/cancel/deadline context $mode" }
      if ($text -notmatch "(?m)^TCP_PRIORITY_OWNER_CENSUS mode=$mode handles=([0-9]+)/\1 threads=([0-9]+)/\2\r?$") { throw "Missing HOST repeated physical owner census $mode" }
'''
OWNER_THREAD_CONTROLLER_AFTER=r'''  if ($Context -eq 'host-posix') {
    if ($text -notmatch '(?m)^TCP_PRIORITY_OWNER_CLASSIFIER default_pool=1 ordinary_thread_rejected=1 exact_retirement=1\r?$') { throw 'Missing HOST calibrated pool and ordinary-thread negative control' }
    foreach ($mode in 0..3) {
      if ($text -notmatch "(?m)^TCP_PRIORITY_OWNED_WAIT mode=$mode .*native_retired=1\r?$") { throw "Missing HOST actual mode/cancel/deadline context $mode" }
      if ($text -notmatch "(?m)^TCP_PRIORITY_OWNER_CENSUS mode=$mode handles=([0-9]+)/\1 threads=[0-9]+/[0-9]+\r?$") { throw "Missing HOST exact process handle census $mode" }
      if ($text -notmatch "(?m)^TCP_PRIORITY_OWNER_IDENTITIES mode=$mode entries=([1-9][0-9]*)/\1 pool=[0-9]+/[0-9]+ matched=1\r?$") { throw "Missing HOST exact non-pool thread identities $mode" }
      if ($text -notmatch "(?m)^TCP_PRIORITY_OWNED_REFS mode=$mode operation=0 description=0\r?$") { throw "Missing HOST expired product owner references $mode" }
'''
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
# This exact transition is applied in reverse to the current OFD and SCM_RIGHTS bytes,
# then EVERY earlier preservation assertion below still runs on its result.
# Coupled OFD/stdio/pipe source files are pinned by the same qualification.
# No supplier, socket core, fixture predicate, deadline or DAG is exempted.
OFD_QUALIFICATION_SHA256 = 'caeea5f5dd515a2522011a0a1f0dc5271dadfa8929e87ac1393f668bf096e49b'
# Shared line input changes only these five complete sources. Reverse their
# exact pinned transition before the original OFD preservation checks below;
# retain that earlier manifest, all its digests and its runtime approval state.
# This source admission does not stand in for the Windows/native CI gates.
STDIO_LINE_QUALIFICATION_SHA256='83c559c6a16a1088013af39dc8938b61d2fb8a66ba9a5cc946956b7622735b31'
STDIO_LINE_PATHS=['compat/bionic/stdio_bionic.cc','compat/posix/include/gesher/stdio.h',
                  'compat/posix/src/misc.cc','compat/posix/src/stdio_output.cc',
                  'compat/posix/src/stdio_stream_buffer.h']
def preceding_stdio_line_sources(current):
 folder=ROOT/'tools/net-tcp-priority/qualified'
 raw=(folder/'stdio-line-source.json').read_bytes()
 assert hashlib.sha256(raw).hexdigest()==STDIO_LINE_QUALIFICATION_SHA256,'stdio line source qualification changed'
 qualification=json.loads(raw)
 assert qualification['baseline']=='6afcb6e671fc4f2d08a4a98220f9d9a906a327bf'
 assert qualification['native_runtime_approval'] is False
 assert qualification['paths']==STDIO_LINE_PATHS
 assert list(qualification['source_sha256'])==list(qualification['previous_sha256'])==STDIO_LINE_PATHS
 patch=(folder/'stdio-line-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==qualification['patch_sha256'],'stdio line transition patch changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==STDIO_LINE_PATHS
 assert list(current)==STDIO_LINE_PATHS,'stdio line historical paths changed'
 for path,actual in current.items():
  assert hashlib.sha256(actual).hexdigest()==qualification['source_sha256'][path],path
 with tempfile.TemporaryDirectory(prefix='gesher-stdio-line-qualification-') as temporary:
  work=Path(temporary)
  for path,actual in current.items():
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(actual)
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  previous={path:(work/path).read_bytes() for path in STDIO_LINE_PATHS}
  for path,actual in previous.items():
   expected=subprocess.check_output(['git','-C',str(ROOT),'show',qualification['baseline']+':'+path])
   assert actual==expected and hashlib.sha256(actual).hexdigest()==qualification['previous_sha256'][path],path
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert all((work/path).read_bytes()==actual for path,actual in current.items()),'stdio line forward reconstruction changed'
 return previous

# Reverse only the pinned scanner adapter transition before all original OFD
# checks. This source proof grants no supplier, controller or runtime approval.
STDIO_SCAN_QUALIFICATION_SHA256='3c08f7f0ea996532377f7d4930ce2d0e84320f3339bd26eed8f7a4b89aad36d0'
def preceding_stdio_scan_source(current):
 folder=ROOT/'tools/net-tcp-priority/qualified'
 raw=(folder/'stdio-scan-source.json').read_bytes()
 assert hashlib.sha256(raw).hexdigest()==STDIO_SCAN_QUALIFICATION_SHA256,'stdio scan source qualification changed'
 qualification=json.loads(raw)
 path='compat/posix/src/stdio_output.cc'
 assert qualification['baseline']=='d38b5549dc3963f8d491e9205f9b2743bb6d3745'
 assert qualification['native_runtime_approval'] is False
 assert qualification['paths']==[path]
 assert list(qualification['source_sha256'])==list(qualification['previous_sha256'])==[path]
 patch=(folder/'stdio-scan-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==qualification['patch_sha256'],'stdio scan transition patch changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==[path]
 assert hashlib.sha256(current).hexdigest()==qualification['source_sha256'][path],path
 with tempfile.TemporaryDirectory(prefix='gesher-stdio-scan-qualification-') as temporary:
  work=Path(temporary)
  (work/path).parent.mkdir(parents=True)
  (work/path).write_bytes(current)
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  previous=(work/path).read_bytes()
  expected=subprocess.check_output(['git','-C',str(ROOT),'show',qualification['baseline']+':'+path])
  assert previous==expected and hashlib.sha256(previous).hexdigest()==qualification['previous_sha256'][path],('stdio scan reverse endpoint',path)
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert (work/path).read_bytes()==current,'stdio scan forward reconstruction changed'
 return previous

# Reverse only the exact broker output transition, including its separately
# reviewed zero-length input-copy prerequisite, before all original OFD checks.
# This source proof grants no supplier, controller or native runtime approval.
BINDER_OUTPUT_QUALIFICATION_SHA256='c221504d110bf48da4dab9577a0fc50f9bd0867a553e3bb9042a2eda82cafe64'
def preceding_binder_output_source(current=None):
 folder=ROOT/'tools/net-tcp-priority/qualified'
 raw=(folder/'binder-output-source.json').read_bytes()
 assert hashlib.sha256(raw).hexdigest()==BINDER_OUTPUT_QUALIFICATION_SHA256,'Binder output source qualification changed'
 qualification=json.loads(raw)
 path='compat/binder/binderd.cc'
 assert qualification['baseline']=='5aa6b39027e9c38de2781ff9cda066418d25b1ba'
 assert qualification['native_runtime_approval'] is False
 assert qualification['paths']==[path]
 assert list(qualification['source_sha256'])==list(qualification['previous_sha256'])==[path]
 patch=(folder/'binder-output-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==qualification['patch_sha256'],'Binder output transition patch changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==[path]
 if current is None:current=(ROOT/path).read_bytes()
 assert hashlib.sha256(current).hexdigest()==qualification['source_sha256'][path],path
 with tempfile.TemporaryDirectory(prefix='gesher-binder-output-qualification-') as temporary:
  work=Path(temporary)
  (work/path).parent.mkdir(parents=True)
  (work/path).write_bytes(current)
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  previous=(work/path).read_bytes()
  expected=subprocess.check_output(['git','-C',str(ROOT),'show',qualification['baseline']+':'+path])
  assert previous==expected and hashlib.sha256(previous).hexdigest()==qualification['previous_sha256'][path],path
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert (work/path).read_bytes()==current,'Binder output forward reconstruction changed'
 return previous


# Main's AppContainer admission changes are outside the original output delta.
# Keep the original receipt/patch and validate its historical published source,
# then apply that SAME immutable patch to the separately pinned current-main
# endpoint. Reverse and forward must reproduce every byte at both endpoints;
# the new main OFD ledger remains authoritative for the other coupled sources.
COMPOSED_BINDER_OUTPUT_BASELINE='98215dd7e798090fb5ab6160490ae78f1793ab3a'
COMPOSED_BINDER_OUTPUT_PRIOR_FEATURE='c61fa022341e38ca2c7e88ce229b0e08ac2170d6'
COMPOSED_BINDER_OUTPUT_SHA256='1adfde0ff9fa29299333bc674d2b002f6b2f19d9da450daeb659ae5b391d6f5c'
COMPOSED_BINDER_OUTPUT_PREVIOUS_SHA256='bf09a9ae3dc590806f27951dc8f9d7df3aa468be958cac1ee47e4467e4fef79c'
COMPOSED_BINDER_OUTPUT_PATCH_SHA256='1864085a254a4aaee67b3c6fa3fe4f8c3a8012596b5c1fac7fb3d953bcce6d36'
def preceding_composed_binder_output_source(current):
 path='compat/binder/binderd.cc'
 historical=subprocess.check_output(['git','-C',str(ROOT),'show',COMPOSED_BINDER_OUTPUT_PRIOR_FEATURE+':'+path])
 preceding_binder_output_source(historical)
 assert hashlib.sha256(current).hexdigest()==COMPOSED_BINDER_OUTPUT_SHA256,'composed Binder source changed'
 expected=subprocess.check_output(['git','-C',str(ROOT),'show',COMPOSED_BINDER_OUTPUT_BASELINE+':'+path])
 assert hashlib.sha256(expected).hexdigest()==COMPOSED_BINDER_OUTPUT_PREVIOUS_SHA256,'composed Binder baseline changed'
 patch=(ROOT/'tools/net-tcp-priority/qualified/binder-output-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==COMPOSED_BINDER_OUTPUT_PATCH_SHA256,'composed Binder original patch changed'
 with tempfile.TemporaryDirectory(prefix='gesher-composed-binder-output-qualification-') as temporary:
  work=Path(temporary)
  (work/path).parent.mkdir(parents=True)
  (work/path).write_bytes(current)
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  previous=(work/path).read_bytes()
  assert previous==expected,'composed Binder reverse endpoint changed'
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert (work/path).read_bytes()==current,'composed Binder forward endpoint changed'
 return previous


# PR73 unifies Bionic conversion metadata with the existing shared stream
# owner. Keep the original OFD receipt and all of its 71 source digests intact.
# Only this separately pinned four-file transition may be reversed to main's
# exact bytes before the unchanged OFD and supplier checks run. It neither
# qualifies Windows execution nor replaces review of the production delta.
SHARED_STDIO_BASE = '6d7638b789e21cba6270c1dbfabe5091680c4100'
SHARED_STDIO_FEATURE_HEAD = 'de1e43c391275137c7b7b25dc170892684d55b5e'
SHARED_STDIO_TRANSITION_SHA256 = '8e28669c6d0d077f330d00191846c9b73e40afa12bde0da6ad0bbabd6203f013'
SHARED_STDIO_PATHS = (
 'compat/bionic/stdio_bionic.cc',
 'compat/posix/include/gesher/stdio.h',
 'compat/posix/include/stdio.h',
 'compat/posix/src/stdio_output.cc',
)

def preceding_shared_stdio_sources(qualification,current_sources):
 folder=ROOT/'tools/net-tcp-priority/qualified'
 raw=(folder/'shared-stdio-source.json').read_bytes()
 assert hashlib.sha256(raw).hexdigest()==SHARED_STDIO_TRANSITION_SHA256,'shared-stdio source transition changed'
 transition=json.loads(raw)
 assert set(transition)=={'baseline','feature_head','baseline_policy_sha256','paths',
                         'before_sha256','after_sha256','patch_sha256'},'shared-stdio transition schema changed'
 assert transition['baseline']==SHARED_STDIO_BASE,'shared-stdio baseline changed'
 assert transition['feature_head']==SHARED_STDIO_FEATURE_HEAD,'shared-stdio feature checkpoint changed'
 paths=list(SHARED_STDIO_PATHS)
 assert transition['paths']==paths,'shared-stdio transition paths changed'
 assert sorted(transition['before_sha256'])==sorted(transition['after_sha256'])==paths,'shared-stdio endpoint paths changed'
 baseline_policy=subprocess.check_output(['git','-C',str(ROOT),'show',SHARED_STDIO_BASE+':tools/net-tcp-priority/test-selected-policy.py'])
 assert hashlib.sha256(baseline_policy).hexdigest()==transition['baseline_policy_sha256'],'shared-stdio baseline policy changed'
 patch=(folder/'shared-stdio-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==transition['patch_sha256'],'shared-stdio transition patch changed'
 assert [line for line in patch.splitlines() if line.startswith(b'diff --git ')]==[
     ('diff --git a/'+path+' b/'+path).encode() for path in paths],'shared-stdio patch paths changed'
 assert sorted(current_sources)==paths,'shared-stdio input paths changed'
 current={};baseline={}
 for path in paths:
  current[path]=current_sources[path]
  assert hashlib.sha256(current[path]).hexdigest()==transition['after_sha256'][path],path
  baseline[path]=subprocess.check_output(['git','-C',str(ROOT),'show',SHARED_STDIO_BASE+':'+path])
  assert hashlib.sha256(baseline[path]).hexdigest()==transition['before_sha256'][path],path
  assert transition['before_sha256'][path]==qualification['source_sha256'][path],path
  assert current[path]!=baseline[path],('empty shared-stdio transition',path)
 with tempfile.TemporaryDirectory(prefix='gesher-shared-stdio-qualification-') as temporary:
  work=Path(temporary)
  for path in paths:
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(current[path])
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  for path in paths:
   assert (work/path).read_bytes()==baseline[path],('shared-stdio reverse endpoint',path)
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  for path in paths:
   assert (work/path).read_bytes()==current[path],('shared-stdio forward endpoint',path)
 return baseline



# Compose this leaf with PR73 through a separately pinned current-main delta.
# The original leaf manifest/patch is still checked against its immutable
# published ancestor; neither that receipt nor PR73/OFD approval is broadened.
# Current bytes first reconstruct exact main, then the unchanged PR73 proof
# reconstructs the original OFD sources. Both historical routes must agree.
COMPOSED_STDIO_BASELINE='93c32f6f02521790eaf8936f2ff1fa1a650653f2'
COMPOSED_STDIO_PRIOR_FEATURE='4328a34d04e63f6f3881b34c7dca09ef1fa8ec0b'
COMPOSED_STDIO_PATHS=['compat/bionic/stdio_bionic.cc', 'compat/posix/include/gesher/stdio.h', 'compat/posix/src/misc.cc', 'compat/posix/src/stdio_output.cc', 'compat/posix/src/stdio_stream_buffer.h']
COMPOSED_STDIO_SHA256={'compat/bionic/stdio_bionic.cc': '5bb5e959e3a32cbc2ce0ab594724b7207b6fd921c6310e8c28766faf34bad13a', 'compat/posix/include/gesher/stdio.h': '721085722d4253cfdb4c54bce03c79de5815417b05905316589a22a7d076fdc2', 'compat/posix/src/misc.cc': '5b65a38b98964f89a5f9224c4db5ed4655d5a0e22fce5b0c3cd68138f54ffdfa', 'compat/posix/src/stdio_output.cc': '4a82b1d38908aee96ab6e058ac8a4131705f37045e76781a8ef47de328ffb4e2', 'compat/posix/src/stdio_stream_buffer.h': '8f508a1fe0a389cfdb83d54f90690cedf30fbe1112e31526bf5fb5264deeca8d'}
COMPOSED_STDIO_PREVIOUS_SHA256={'compat/bionic/stdio_bionic.cc': '2ff4ba736e9943b07a555e532682ae2e6ddcaffd115e15f3b1be201c4660b15e', 'compat/posix/include/gesher/stdio.h': '20e0b97b5333daae70d28bb90190e9e4c4f12921561596bdc94cfee38ee47cd7', 'compat/posix/src/misc.cc': '982ff45060b56d1f43cc1580909f9b17ac66889eddf3772f1af52ec763e3648b', 'compat/posix/src/stdio_output.cc': 'b777210b565d8eee802539c2e47c52138159fabe16ca4fdd9fd0c2679118d04f', 'compat/posix/src/stdio_stream_buffer.h': 'f1a990222b0cf289d45c3ee747623368394647d19a221ed1f85ec706b04ce62d'}
COMPOSED_STDIO_PATCH_SHA256='f2c5d659b406965aaaf65c0f69e03f62a5d09deb140f6db1514cd760164cf9af'
def preceding_composed_stdio_sources(current_sources=None):
 current=({path:(ROOT/path).read_bytes() for path in COMPOSED_STDIO_PATHS}
          if current_sources is None else dict(current_sources))
 assert list(current)==COMPOSED_STDIO_PATHS,'composed stdio input paths changed'
 assert list(COMPOSED_STDIO_SHA256)==list(COMPOSED_STDIO_PREVIOUS_SHA256)==COMPOSED_STDIO_PATHS,'composed stdio endpoint paths changed'
 for path,actual in current.items():
  assert hashlib.sha256(actual).hexdigest()==COMPOSED_STDIO_SHA256[path],path
 patch=(ROOT/'tools/net-tcp-priority/qualified/stdio-line-shared-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==COMPOSED_STDIO_PATCH_SHA256,'composed stdio transition changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==COMPOSED_STDIO_PATHS,'composed stdio patch paths changed'
 with tempfile.TemporaryDirectory(prefix='gesher-composed-stdio-qualification-') as temporary:
  work=Path(temporary)
  for path,actual in current.items():
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(actual)
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  previous={path:(work/path).read_bytes() for path in COMPOSED_STDIO_PATHS}
  for path,actual in previous.items():
   expected=subprocess.check_output(['git','-C',str(ROOT),'show',COMPOSED_STDIO_BASELINE+':'+path])
   assert actual==expected and hashlib.sha256(actual).hexdigest()==COMPOSED_STDIO_PREVIOUS_SHA256[path],('composed stdio reverse endpoint',path)
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert all((work/path).read_bytes()==actual for path,actual in current.items()),'composed stdio forward endpoint'
 return previous

# Compose the scanner with the reviewed line-input integration checkpoint.
# The original scanner manifest/patch remains checked against its immutable
# published ancestor, retaining its source proof and runtime approval state.
# Current bytes reconstruct the exact line-input checkpoint; line/shared-stdio
# normalization then restores every original OFD source endpoint below.
COMPOSED_SCAN_BASELINE='dec85ab146a0b95994efa9abd65910693d0019fb'
COMPOSED_SCAN_PRIOR_FEATURE='d38246cb583b669167909902529cb00f4f63e5eb'
COMPOSED_SCAN_PATHS=['compat/posix/src/stdio_output.cc']
COMPOSED_SCAN_SHA256={'compat/posix/src/stdio_output.cc': 'b19de754ba6bf99c122c2ce66418777111ffa3a4e19062a21a3da2145c7d2979'}
COMPOSED_SCAN_PREVIOUS_SHA256={'compat/posix/src/stdio_output.cc': '4a82b1d38908aee96ab6e058ac8a4131705f37045e76781a8ef47de328ffb4e2'}
COMPOSED_SCAN_PATCH_SHA256='ac3844172066ad228ec93f5f33c5874ecbc51ca0de9d1486b807eca46d323d00'
def preceding_composed_scan_sources(current_sources=None):
 current=({path:(ROOT/path).read_bytes() for path in COMPOSED_SCAN_PATHS}
          if current_sources is None else dict(current_sources))
 assert list(current)==COMPOSED_SCAN_PATHS,'composed scan input paths changed'
 assert list(COMPOSED_SCAN_SHA256)==list(COMPOSED_SCAN_PREVIOUS_SHA256)==COMPOSED_SCAN_PATHS,'composed scan endpoint paths changed'
 for path,actual in current.items():
  assert hashlib.sha256(actual).hexdigest()==COMPOSED_SCAN_SHA256[path],path
 patch=(ROOT/'tools/net-tcp-priority/qualified/stdio-scan-shared-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==COMPOSED_SCAN_PATCH_SHA256,'composed scan transition changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==COMPOSED_SCAN_PATHS,'composed scan patch paths changed'
 with tempfile.TemporaryDirectory(prefix='gesher-composed-stdio-qualification-') as temporary:
  work=Path(temporary)
  for path,actual in current.items():
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(actual)
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  previous={path:(work/path).read_bytes() for path in COMPOSED_SCAN_PATHS}
  for path,actual in previous.items():
   expected=subprocess.check_output(['git','-C',str(ROOT),'show',COMPOSED_SCAN_BASELINE+':'+path])
   assert actual==expected and hashlib.sha256(actual).hexdigest()==COMPOSED_SCAN_PREVIOUS_SHA256[path],('composed scan reverse endpoint',path)
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert all((work/path).read_bytes()==actual for path,actual in current.items()),'composed scan forward endpoint'
 return previous

# Normalize the scanner before line input; shared-stdio then receives its
# unchanged complete inputs. Every stage is independently reversible and pinned.
def current_stdio_line_inputs(read_source=live_source):
 scan=preceding_composed_scan_sources({path:read_source(path) for path in COMPOSED_SCAN_PATHS})
 assert SNPRINTF_BOUNDS_SOURCE not in scan,'formatter/scanner source qualifications overlap'
 current={path:scan[path] if path in scan else read_source(path) for path in COMPOSED_STDIO_PATHS}
 current[SNPRINTF_BOUNDS_SOURCE]=preceding_snprintf_bounds_source(read_source)
 return current

def preceding_stdio_feature_sources(read_source=live_source):
 return preceding_composed_stdio_sources(current_stdio_line_inputs(read_source))

# The guest snprintf prefix accumulator changes only this formatter source.
# Reverse its exact transition to the reviewed line-input integration checkpoint.
# The line-input and original shared-stdio transitions then restore every OFD
# digest. This grants no supplier, controller or native runtime approval.
SNPRINTF_BOUNDS_SOURCE='compat/bionic/stdio_bionic.cc'
SNPRINTF_BOUNDS_BASELINE='dec85ab146a0b95994efa9abd65910693d0019fb'
SNPRINTF_BOUNDS_SHA256='96a90eb52ad6e4b91e1ee0e16c526894e5426a16f3a65e61e11e001a1caed75f'
SNPRINTF_BOUNDS_PREVIOUS_SHA256='5bb5e959e3a32cbc2ce0ab594724b7207b6fd921c6310e8c28766faf34bad13a'
SNPRINTF_BOUNDS_PATCH_SHA256='416b44106f476ea5c4dcd519bb8b0c100cb3d614e0afdcef0be334cfd4ec7208'
def preceding_snprintf_bounds_source(read_source=live_source):
 path=SNPRINTF_BOUNDS_SOURCE
 current=read_source(path)
 assert hashlib.sha256(current).hexdigest()==SNPRINTF_BOUNDS_SHA256,path
 patch=(ROOT/'tools/net-tcp-priority/qualified/snprintf-bounds-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==SNPRINTF_BOUNDS_PATCH_SHA256,'snprintf bounds transition changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==[path]
 with tempfile.TemporaryDirectory(prefix='gesher-snprintf-qualification-') as temporary:
  work=Path(temporary)
  (work/path).parent.mkdir(parents=True)
  (work/path).write_bytes(current)
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  previous=(work/path).read_bytes()
  expected=subprocess.check_output(['git','-C',str(ROOT),'show',SNPRINTF_BOUNDS_BASELINE+':'+path])
  assert previous==expected and hashlib.sha256(previous).hexdigest()==SNPRINTF_BOUNDS_PREVIOUS_SHA256,('snprintf bounds reverse endpoint',path)
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert (work/path).read_bytes()==current,'snprintf bounds forward reconstruction changed'
 return previous
# NATIVE-13 adds a revision snapshot and allocation-free area lookup to the
# property file already qualified with #118's shared environment owner. Reverse
# only that exact transition to main 1a3a5c244d643c7c93b621326857c6b8c858d5bb;
# the original OFD receipt and every one of its hashes remain unchanged below.
# This source qualification grants no supplier, controller or runtime approval.
PROPERTY_REVISION_SHA256 = '2a4fd531e6609b7705710c212d7772e1e94c41d3618323f5641852fb62fe4c24'
PROPERTY_REVISION_HEADER_SHA256 = '9832b7d1282efb24bf0cf621b3dc8cb1c1279e0988b7895add146e3193364ac3'
PROPERTY_REVISION_PATCH_SHA256 = '5bb1443e1964494445e1818a9d6116979e095b23856fcf7a96f0850f865eab27'
# #421 makes the property area's build.prop loader keep ro.opengles.version only
# as a version Gesher serves (gesher/gles_version_property.h): one include and
# one filter line in Initial(). Reverse only that exact transition to the bytes
# PROPERTY_REVISION_SHA256 pins, then the NATIVE-13 transition below as before.
# This source qualification grants no supplier, controller or runtime approval.
GLES_PROPERTY_SHA256 = '4d7926dd316851b91bd45620a967da119b54e2408c44e351303ab937866f440c'
GLES_PROPERTY_PATCH = 'tools/net-tcp-priority/qualified/gles-property-source.patch'
GLES_PROPERTY_PATCH_SHA256 = '72a6c1c9ba827072ba13c9aac32774e4dcda74260d54f8853e25ae7971be06f6'
def preceding_gles_property_source(source):
 path='compat/posix/src/system_properties.cc'
 assert hashlib.sha256(source).hexdigest()==GLES_PROPERTY_SHA256,path
 patch=(ROOT/GLES_PROPERTY_PATCH).read_bytes()
 assert hashlib.sha256(patch).hexdigest()==GLES_PROPERTY_PATCH_SHA256,'gles property transition changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==[path]
 with tempfile.TemporaryDirectory(prefix='gesher-gles-property-qualification-') as temporary:
  work=Path(temporary)
  (work/path).parent.mkdir(parents=True)
  (work/path).write_bytes(source)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','-'],input=patch,cwd=work,check=True,capture_output=True)
  previous=(work/path).read_bytes()
  # The transition applied forward to the reversed bytes reproduces every current byte.
  subprocess.run(['git','apply','--unidiff-zero','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert (work/path).read_bytes()==source,'gles property transition does not reproduce: '+path
 return previous
def preceding_logging_property_source(source, header_source):
 path='compat/posix/src/system_properties.cc'
 source=preceding_gles_property_source(source)
 assert hashlib.sha256(source).hexdigest()==PROPERTY_REVISION_SHA256,path
 header='compat/posix/include/gesher/system_properties.h'
 assert hashlib.sha256(header_source).hexdigest()==PROPERTY_REVISION_HEADER_SHA256,header
 patch=(ROOT/'tools/net-tcp-priority/qualified/native13-property-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==PROPERTY_REVISION_PATCH_SHA256,'property revision transition changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==[path]
 with tempfile.TemporaryDirectory(prefix='gesher-property-qualification-') as temporary:
  work=Path(temporary)
  (work/path).parent.mkdir(parents=True)
  (work/path).write_bytes(source)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','-'],input=patch,cwd=work,check=True,capture_output=True)
  return (work/path).read_bytes()
# Releasing a memfd's writable-mapping claim (a LockFileEx lease that lives as
# long as the writable shared mapping) changes three sources the OFD receipt
# pins. Reverse only that exact transition to main's bytes at its baseline; the
# original OFD receipt and every one of its hashes remain unchanged below.
# This source qualification grants no supplier, controller or runtime approval.
MEMFD_LEASE_QUALIFICATION='tools/net-tcp-priority/qualified/memfd-lease-source.json'
MEMFD_LEASE_PATCH='tools/net-tcp-priority/qualified/memfd-lease-source.patch'
MEMFD_LEASE_QUALIFICATION_SHA256 = '6cf8d4bbad04834885426b63c468f0f7f4b1b64784190ed8cab523bbf082ae8c'
MEMFD_LEASE_BASELINE='e074b67b2f83ce9128c11713171329e9fbbf6739'
MEMFD_LEASE_PATHS=['compat/posix/src/internal.h','compat/posix/src/memfd_seals.cc','compat/posix/src/mman.cc']
def preceding_memfd_lease_sources(usb_block,read_source=live_source):
 raw=(ROOT/MEMFD_LEASE_QUALIFICATION).read_bytes()
 assert hashlib.sha256(raw).hexdigest()==MEMFD_LEASE_QUALIFICATION_SHA256,'memfd lease source qualification changed'
 qualification=json.loads(raw)
 assert qualification['baseline']==MEMFD_LEASE_BASELINE and qualification['native_runtime_approval'] is False
 assert qualification['paths']==MEMFD_LEASE_PATHS
 assert sorted(qualification['previous_sha256'])==sorted(qualification['source_sha256'])==sorted(MEMFD_LEASE_PATHS)
 patch=(ROOT/MEMFD_LEASE_PATCH).read_bytes()
 assert hashlib.sha256(patch).hexdigest()==qualification['patch_sha256'],'memfd lease transition patch changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==MEMFD_LEASE_PATHS
 # internal.h also carries this PR's independently pinned USB declarations.
 # Remove only that exact USB delta first; the unchanged memfd receipt must
 # then see every byte of main's lease implementation before reversing it.
 assert set(usb_block).intersection(MEMFD_LEASE_PATHS)=={'compat/posix/src/internal.h'},'USB/memfd qualification overlap changed'
 current={path:usb_block[path] if path in usb_block else read_source(path)
          for path in MEMFD_LEASE_PATHS}
 for path in MEMFD_LEASE_PATHS:
  assert hashlib.sha256(current[path]).hexdigest()==qualification['source_sha256'][path],path
 with tempfile.TemporaryDirectory(prefix='gesher-memfd-lease-qualification-') as temporary:
  work=Path(temporary)
  for path in MEMFD_LEASE_PATHS:
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(current[path])
  subprocess.run(['git','apply','--reverse','--unidiff-zero','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','-'],input=patch,cwd=work,check=True,capture_output=True)
  previous={path:(work/path).read_bytes() for path in MEMFD_LEASE_PATHS}
  # The reversed bytes are exactly the baseline's, and the transition applied
  # forward to them reproduces every current byte.
  for path in MEMFD_LEASE_PATHS:
   assert hashlib.sha256(previous[path]).hexdigest()==qualification['previous_sha256'][path],'memfd lease reverse transition is not the baseline: '+path
   assert previous[path]==subprocess.check_output(['git','-C',str(ROOT),'show',MEMFD_LEASE_BASELINE+':'+path]),'memfd lease reverse transition is not the baseline: '+path
  subprocess.run(['git','apply','--unidiff-zero','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--unidiff-zero','-'],input=patch,cwd=work,check=True,capture_output=True)
  for path in MEMFD_LEASE_PATHS:assert (work/path).read_bytes()==current[path],'memfd lease transition does not reproduce: '+path
 return previous
# Bionic's independently reviewed memmem search changes only this include and
# complete wrapper. Restore those exact bytes before applying the UNCHANGED
# OFD qualification digest. Pin the new pure-search helper separately: no other
# shim byte, supplier, fixture, deadline or runtime gate is exempted.
MEMMEM_HEADER='compat/bionic/memmem_search.h'
MEMMEM_HEADER_SHA256='ae673530674a38f0fc9a0752ef138c8f9d22a2ba08cdf29a0032f6a622f3cdf0'
MEMMEM_REPLACEMENTS=(
 (b'#include "extra_bionic.h"\n\n#include <gesher/bionic_shim.h>\n',
  b'#include "extra_bionic.h"\n#include "memmem_search.h"\n\n#include <gesher/bionic_shim.h>\n'),
 (b'''void* BIONIC_SYSV Memmem(const void* h, size_t hn, const void* n, size_t nn) {
  if (nn == 0) return const_cast<void*>(h);
  auto hp = static_cast<const uint8_t*>(h);
  for (size_t i = 0; i + nn <= hn; ++i) {
    if (std::memcmp(hp + i, n, nn) == 0) return const_cast<uint8_t*>(hp + i);
  }
  return nullptr;
}
''',
  b'''void* BIONIC_SYSV Memmem(const void* h, size_t hn, const void* n, size_t nn) {
  if (nn == 0) return const_cast<void*>(h);
  return const_cast<void*>(search::FindBytes(h, hn, n, nn));
}
'''),
)
def preceding_memmem_source(actual):
 assert hashlib.sha256((ROOT/MEMMEM_HEADER).read_bytes()).hexdigest()==MEMMEM_HEADER_SHA256,MEMMEM_HEADER
 for before,after in MEMMEM_REPLACEMENTS:
  assert actual.count(after)==1 and before not in actual,'exact reviewed memmem substitution changed'
  actual=actual.replace(after,before,1)
 return actual
# Valid pipes can now fail a control operation explicitly. These two exact
# non-socket dispatch substitutions are the only PollFdsImpl changes allowed;
# normalize them before comparing every byte of the original native poll core.
PIPE_POLL_ERROR_REPLACEMENTS = (
 (b'''    } else if (!PollFile(p)) {
      waiting_files = true;
''', b'''    } else {
      int result = PollFile(p);
      if (result < 0) return -1;
      if (!result) waiting_files = true;
'''),
 (b'''        if (!PollFile(p)) waiting_files = true;
''', b'''        int result = PollFile(p);
        if (result < 0) return -1;
        if (!result) waiting_files = true;
'''),
)
# Preserve the original OFD evidence while reviewing the USB block device
# dispatch and contained runtime changes separately. The reverse patch
# is checked against historical main bytes; the socket ownership and poll
# checks still compare live code.
USB_BLOCK_BASE = '08146b1ea150229313ef14a8fcd99df217a57fb9'
# These additional runtime files already contained reviewed changes after the
# original USB baseline. Reverse only this PR's new edits in those files, then
# require the existing OFD receipt's exact bytes as before.
USB_BLOCK_PATH_BASES = dict.fromkeys((
 'compat/bionic/process_bionic.cc',
 'compat/elf/exec/exec_spawn.h',
 'compat/elf/exec/gesher_exec.cc',
 'compat/libcore/process.cc',
 'compat/posix/src/binder_file_descriptions.h',
 'compat/posix/src/binder_wire.h',
 'tests/native/ofd_transfer_host_test.cc.in',
 'tools/test_environment_lookup_contracts.py',
), 'af2a394d0f5399f954aa00aeaf37011d42b96623')
# Main's native-fence ioctl and shell/exec repairs have their own updated OFD
# receipt. Reverse USB changes in these overlapping files to that exact main
# checkpoint, preserving those repairs before the original checks run.
USB_BLOCK_PATH_BASES.update(dict.fromkeys((
 'compat/elf/exec/exec_spawn.cc',
 'compat/elf/exec/exec_spawn.h',
 'compat/elf/exec/gesher_shell.cc',
 'compat/elf/exec/shell_execute.inc',
 'compat/libcore/process.cc',
 'compat/posix/src/system.cc',
), '5b2af6e4e5de5525791c304de21c2c6a77aebcd2'))
# Preserve main's memfd lease declarations before their separately pinned
# reverse transition is checked against the original OFD receipt.
USB_BLOCK_PATH_BASES['compat/posix/src/internal.h'] = 'dcc29a49143c4bd33e0e60a81b25e0e1f2cfc12a'
# Re-pin files updated in main through a62bd9a0a (arm64 fork, FIFO, etc.).
USB_BLOCK_PATH_BASES.update(dict.fromkeys((
 'compat/bionic/process_bionic.cc',
 'compat/elf/exec/exec_spawn.cc',
 'compat/elf/exec/exec_spawn.h',
 'compat/elf/exec/gesher_exec.cc',
 'compat/posix/posix.cmake',
 'compat/posix/src/binder.cc',
 'compat/posix/src/fd.cc',
 'compat/posix/src/fsops.cc',
 'compat/posix/src/internal.h',
 'compat/posix/src/ofd.cc',
 'compat/posix/src/ofd_internal.h',
 'compat/posix/src/procfs.cc',
 'compat/posix/src/system.cc',
 'compat/posix/src/system_properties.cc',
 'compat/posix/src/unistd.cc',
 'compat/posix/src/unix_rights.cc',
 'tests/native/ofd_transfer_host_test.cc.in',
), 'a62bd9a0a9bd5bcd5f428929dc52842fb938ac5a'))
USB_BLOCK_PATH_BASES.update(dict.fromkeys((
 'compat/posix/src/fsops.cc',
), 'a44f4d055348db76345a4698441f6301930e401c'))
# Main's scoped-storage transition is qualified after reversing USB edits.
USB_BLOCK_PATH_BASES.update(dict.fromkeys((
 'compat/art/dalvikvm_main.cc',
 'compat/elf/exec/exec_spawn.cc',
 'compat/elf/exec/gesher_exec.cc',
 'compat/posix/posix.cmake',
 'compat/posix/src/fsops.cc',
 'compat/posix/src/storage_policy.cc',
 'compat/posix/src/system.cc',
 'compat/posix/src/unistd.cc',
 'tests/art-temp/entry_test.cc',
 'tests/native/ofd_transfer_host_test.cc.in',
 'tools/test_temp_directory.py',
), '43edc832893d4cd21843c7c6d56424556b625016'))
# Main #513 changes the exact intermediate procfs/OFD/storage bytes underneath
# USB. Keep every historical USB path anchor above, and prove this separately
# pinned main transition to d631 in both directions before removing USB edits.
# The incoming main receipts then see their exact reviewed bytes; their original
# OFD/memfd/storage baselines and approval boundaries are unchanged.
USB_MAIN513_QUALIFICATION='tools/net-tcp-priority/qualified/usb-main513-source.json'
USB_MAIN513_PATCH='tools/net-tcp-priority/qualified/usb-main513-source.patch'
USB_MAIN513_QUALIFICATION_SHA256='f65441e94acb1064d97d2a78f2161ed47f543844833707dcfd31d530de6b3b78'
USB_MAIN513_BASE='156abd94bbe733e07171931027d5eb3613565dc1'
USB_MAIN513_COMMIT='d63126a95129f4e9bad54676b8e076b12542c86b'
USB_MAIN513_PATHS=[
 'compat/posix/src/fsops.cc',
 'compat/posix/src/internal.h',
 'compat/posix/src/ofd_internal.h',
 'compat/posix/src/procfs.cc',
 'compat/posix/src/unistd.cc',
]
def qualified_usb_main513_endpoints():
 raw=(ROOT/USB_MAIN513_QUALIFICATION).read_bytes()
 assert hashlib.sha256(raw).hexdigest()==USB_MAIN513_QUALIFICATION_SHA256,'USB main513 qualification changed'
 transition=json.loads(raw)
 assert set(transition)=={'base','commit','paths','path_bases','before_sha256','after_sha256',
                          'patch_sha256','native_runtime_approval'},'USB main513 schema changed'
 assert transition['base']==USB_MAIN513_BASE and transition['commit']==USB_MAIN513_COMMIT
 assert transition['native_runtime_approval'] is False
 assert transition['paths']==USB_MAIN513_PATHS
 assert transition['path_bases']=={p:USB_BLOCK_PATH_BASES.get(p,USB_BLOCK_BASE) for p in USB_MAIN513_PATHS}
 assert sorted(transition['before_sha256'])==sorted(transition['after_sha256'])==USB_MAIN513_PATHS
 assert subprocess.check_output(['git','-C',str(ROOT),'show','-s','--format=%P',USB_MAIN513_COMMIT]).decode().strip()==USB_MAIN513_BASE
 delta=(ROOT/USB_MAIN513_PATCH).read_bytes()
 assert hashlib.sha256(delta).hexdigest()==transition['patch_sha256'],'USB main513 patch changed'
 assert [line.decode().split(' b/',1)[1] for line in delta.splitlines() if line.startswith(b'diff --git ')]==USB_MAIN513_PATHS
 before={p:subprocess.check_output(['git','-C',str(ROOT),'show',USB_BLOCK_PATH_BASES.get(p,USB_BLOCK_BASE)+':'+p])
         for p in USB_MAIN513_PATHS}
 after={p:subprocess.check_output(['git','-C',str(ROOT),'show',USB_MAIN513_COMMIT+':'+p])
        for p in USB_MAIN513_PATHS}
 for p in USB_MAIN513_PATHS:
  assert hashlib.sha256(before[p]).hexdigest()==transition['before_sha256'][p],('USB main513 historical endpoint',p)
  assert hashlib.sha256(after[p]).hexdigest()==transition['after_sha256'][p],('USB main513 incoming endpoint',p)
  assert before[p]!=after[p],('empty USB main513 transition',p)
 with tempfile.TemporaryDirectory(prefix='gesher-usb-main513-qualification-') as temporary:
  work=Path(temporary)
  for p in USB_MAIN513_PATHS:
   (work/p).parent.mkdir(parents=True,exist_ok=True)
   (work/p).write_bytes(after[p])
  subprocess.run(['git','apply','--reverse','--unidiff-zero','--check','-'],input=delta,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','-'],input=delta,cwd=work,check=True,capture_output=True)
  for p in USB_MAIN513_PATHS:assert (work/p).read_bytes()==before[p],('USB main513 reverse endpoint',p)
  subprocess.run(['git','apply','--unidiff-zero','--check','-'],input=delta,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--unidiff-zero','-'],input=delta,cwd=work,check=True,capture_output=True)
  for p in USB_MAIN513_PATHS:assert (work/p).read_bytes()==after[p],('USB main513 forward endpoint',p)
 return after


USB_BLOCK_TRANSITION_SHA256 = 'c2d21ff03f9a6a1d9cb8a095450ff7fa72a20860d6b60398ef0aaf5b49068676'
USB_NETLINK_MAIN = '279966e6bcb842ae8a44fef87d740baed6818208'
USB_NETLINK_PATHS = {
 'compat/posix/posix.cmake',
 'compat/posix/src/fd.cc',
 'compat/posix/src/system.cc',
}
USB_BLOCK_PATH_BASES['compat/posix/include/gesher/file_description.h'] = '82bb674a00ed50fcfa5c234fcd7f9830d08d443e'
USB_BLOCK_PATH_BASES['compat/binder/binderd.cc'] = 'a025ba60e321a0238c0da90808e2ec9cc69f64ca'
USB_BLOCK_TRANSITION_PATHS = (
 'compat/art/dalvikvm_main.cc',
 'compat/binder/binderd.cc',
 'compat/bionic/process_bionic.cc',
 'compat/elf/exec/exec_spawn.cc',
 'compat/elf/exec/exec_spawn.h',
 'compat/elf/exec/gesher_exec.cc',
 'compat/elf/exec/gesher_shell.cc',
 'compat/elf/exec/shell_execute.inc',
 'compat/libcore/process.cc',
 'compat/posix/include/gesher/file_description.h',
 'compat/posix/posix.cmake',
 'compat/posix/src/binder.cc',
 'compat/posix/src/binder_file_descriptions.h',
 'compat/posix/src/binder_wire.h',
 'compat/posix/src/fd.cc',
 'compat/posix/src/fsops.cc',
 'compat/posix/src/internal.h',
 'compat/posix/src/ofd.cc',
 'compat/posix/src/ofd_internal.h',
 'compat/posix/src/procfs.cc',
 'compat/posix/src/storage_policy.cc',
 'compat/posix/src/system.cc',
 'compat/posix/src/system_properties.cc',
 'compat/posix/src/unistd.cc',
 'compat/posix/src/unix_rights.cc',
 'tests/art-temp/entry_test.cc',
 'tests/native/ofd_transfer_host_test.cc.in',
 'tools/test_environment_lookup_contracts.py',
 'tools/test_temp_directory.py',
)


def preceding_usb_block_sources(folder,read_source=live_source):
 main513 = qualified_usb_main513_endpoints()
 raw = (folder/'usb-block-source.json').read_bytes()
 assert hashlib.sha256(raw).hexdigest() == USB_BLOCK_TRANSITION_SHA256, 'USB block source transition changed'
 transition = json.loads(raw)
 paths = list(USB_BLOCK_TRANSITION_PATHS)
 assert set(transition) == {'base', 'path_bases', 'patch_sha256', 'after_sha256', 'dependencies_sha256'}
 assert transition['base'] == USB_BLOCK_BASE
 assert transition['path_bases'] == USB_BLOCK_PATH_BASES
 assert sorted(transition['after_sha256']) == paths
 delta = (folder/'usb-block-source.patch').read_bytes()
 assert hashlib.sha256(delta).hexdigest() == transition['patch_sha256']
 assert [line for line in delta.splitlines() if line.startswith(b'diff --git ')] == [
     ('diff --git a/'+path+' b/'+path).encode() for path in paths]
 for path, digest in transition['dependencies_sha256'].items():
  assert hashlib.sha256(read_source(path)).hexdigest() == digest, path

 preceding = {}
 with tempfile.TemporaryDirectory(prefix='gesher-usb-block-qualification-') as temporary:
  work = Path(temporary)
  for path in paths:
   current = read_source(path)
   assert hashlib.sha256(current).hexdigest() == transition['after_sha256'][path], path
   (work/path).parent.mkdir(parents=True, exist_ok=True)
   (work/path).write_bytes(current)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','--check','-'],
                 input=delta,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','-'],
                 input=delta,cwd=work,check=True,capture_output=True)
  for path in paths:
   preceding[path] = (work/path).read_bytes()
   base = USB_NETLINK_MAIN if path in USB_NETLINK_PATHS else USB_BLOCK_PATH_BASES.get(path, USB_BLOCK_BASE)
   historical = main513[path] if path in main513 else subprocess.check_output(
       ['git','-C',str(ROOT),'show',base+':'+path])
   assert preceding[path] == historical, path
  subprocess.run(['git','apply','--unidiff-zero','--check','-'],input=delta,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--unidiff-zero','-'],input=delta,cwd=work,check=True,capture_output=True)
  for path in paths:assert (work/path).read_bytes()==read_source(path),('USB forward endpoint',path)
 return preceding


# Storage policy adds file-path checks and launch initialization to files also
# covered by the OFD receipt. Reverse this separately pinned transition to the
# exact main checkpoint before running EVERY original receipt and TCP check.
# The helper pins bind the new references to their reviewed source bytes only:
# neither this transition nor a passing source check qualifies native execution.
# Route netlink extends the socket path covered by the existing receipts.
# Reverse its exact delta before running those checks on their original inputs.
NETLINK_PATCH='tools/net-tcp-priority/qualified/netlink-source.patch'
NETLINK_RECEIPT='tools/net-tcp-priority/qualified/netlink-source.json'
NETLINK_RECEIPT_SHA256='585c1908cd44fb873cf65f24f70c8b78f68ae15e170aae3134e7e4ae5b152ed1'

def preceding_netlink_sources(usb_block,read_source=live_source):
 raw=(ROOT/NETLINK_RECEIPT).read_bytes()
 assert hashlib.sha256(raw).hexdigest()==NETLINK_RECEIPT_SHA256,NETLINK_RECEIPT
 receipt=json.loads(raw)
 patch=(ROOT/NETLINK_PATCH).read_bytes()
 assert hashlib.sha256(patch).hexdigest()==receipt['patch_sha256'],NETLINK_PATCH
 paths=sorted(receipt['after_sha256'])
 assert paths==sorted(receipt['before_sha256'])
 assert set(usb_block).intersection(paths)==USB_NETLINK_PATHS,'USB/Netlink qualification overlap changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==paths
 current={path:usb_block[path] if path in usb_block else read_source(path) for path in paths}
 with tempfile.TemporaryDirectory(prefix='gesher-netlink-qualification-') as temporary:
  work=Path(temporary)
  for path,source in current.items():
   assert hashlib.sha256(source).hexdigest()==receipt['after_sha256'][path],path
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(source)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','-'],input=patch,cwd=work,check=True,capture_output=True)
  before={path:(work/path).read_bytes() for path in paths}
  for path,source in before.items():
   assert hashlib.sha256(source).hexdigest()==receipt['before_sha256'][path],path
   assert source==subprocess.check_output(['git','-C',str(ROOT),'show',receipt['baseline']+':'+path]),path
   if path in USB_NETLINK_PATHS:
    usb_base=USB_BLOCK_PATH_BASES.get(path,USB_BLOCK_BASE)
    assert source==subprocess.check_output(['git','-C',str(ROOT),'show',usb_base+':'+path]),('Netlink reverse and original USB endpoints disagree',path)
  subprocess.run(['git','apply','--unidiff-zero','-'],input=patch,cwd=work,check=True,capture_output=True)
  for path,source in current.items():
   assert (work/path).read_bytes()==source,path
 return before

STORAGE_POLICY_QUALIFICATION='tools/net-tcp-priority/qualified/storage-policy-source.json'
STORAGE_POLICY_PATCH='tools/net-tcp-priority/qualified/storage-policy-source.patch'
STORAGE_POLICY_QUALIFICATION_SHA256='1274a14dce7607f2ea1a6eea833f4036a48a8fe306b58e799cb04080e613cc41'
STORAGE_POLICY_BASELINE='43e210f4eb7505b8e118044e28d4870433bbb23f'
STORAGE_POLICY_PATHS=[
 'compat/art/dalvikvm_main.cc',
 'compat/elf/exec/exec_spawn.cc',
 'compat/elf/exec/gesher_exec.cc',
 'compat/posix/posix.cmake',
 'compat/posix/src/fsops.cc',
 'compat/posix/src/socket.cc',
 'compat/posix/src/system.cc',
 'compat/posix/src/unistd.cc',
 'tests/art-temp/entry_test.cc',
 'tests/native/ofd_transfer_host_test.cc.in',
 'tools/test_temp_directory.py',
]
STORAGE_POLICY_HELPERS=[
 'compat/posix/include/gesher/storage_policy.h',
 'compat/posix/src/storage_policy.cc',
 'compat/posix/src/storage_policy.h',
]
def preceding_storage_policy_sources(before_features,read_source=live_source):
 raw=(ROOT/STORAGE_POLICY_QUALIFICATION).read_bytes()
 assert hashlib.sha256(raw).hexdigest()==STORAGE_POLICY_QUALIFICATION_SHA256,'storage policy source qualification changed'
 qualification=json.loads(raw)
 assert set(qualification)=={'baseline','baseline_policy_sha256','paths','previous_sha256',
                             'source_sha256','helper_sha256','patch_sha256','native_runtime_approval'},'storage policy transition schema changed'
 assert qualification['baseline']==STORAGE_POLICY_BASELINE and qualification['native_runtime_approval'] is False
 assert qualification['paths']==STORAGE_POLICY_PATHS
 assert sorted(qualification['previous_sha256'])==sorted(qualification['source_sha256'])==STORAGE_POLICY_PATHS
 assert sorted(qualification['helper_sha256'])==STORAGE_POLICY_HELPERS
 baseline_policy=subprocess.check_output(['git','-C',str(ROOT),'show',STORAGE_POLICY_BASELINE+':tools/net-tcp-priority/test-selected-policy.py'])
 assert hashlib.sha256(baseline_policy).hexdigest()==qualification['baseline_policy_sha256'],'storage policy baseline policy changed'
 for path in STORAGE_POLICY_HELPERS:
  assert hashlib.sha256(before_features.get(path,read_source(path))).hexdigest()==qualification['helper_sha256'][path],path
 patch=(ROOT/STORAGE_POLICY_PATCH).read_bytes()
 assert hashlib.sha256(patch).hexdigest()==qualification['patch_sha256'],'storage policy transition patch changed'
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==STORAGE_POLICY_PATHS
 current={path:before_features[path] if path in before_features else read_source(path)
          for path in STORAGE_POLICY_PATHS}
 baseline={path:subprocess.check_output(['git','-C',str(ROOT),'show',STORAGE_POLICY_BASELINE+':'+path]) for path in STORAGE_POLICY_PATHS}
 for path in STORAGE_POLICY_PATHS:
  assert hashlib.sha256(current[path]).hexdigest()==qualification['source_sha256'][path],path
  assert hashlib.sha256(baseline[path]).hexdigest()==qualification['previous_sha256'][path],path
  assert current[path]!=baseline[path],('empty storage policy transition',path)
 with tempfile.TemporaryDirectory(prefix='gesher-storage-policy-qualification-') as temporary:
  work=Path(temporary)
  for path in STORAGE_POLICY_PATHS:
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(current[path])
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  for path in STORAGE_POLICY_PATHS:
   assert (work/path).read_bytes()==baseline[path],('storage policy reverse endpoint',path)
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  for path in STORAGE_POLICY_PATHS:
   assert (work/path).read_bytes()==current[path],('storage policy forward endpoint',path)
 return baseline

# Reverse only the reviewed adaptive stable-directory sort before the exact
# Memrchr transition and all original OFD checks. It grants no supplier,
# controller or runtime approval and admits no other POSIX source change.
SCANDIR_BASE = '5894cea6c2bc35d1ae316f8bb401181689a85424'
SCANDIR_SOURCE = 'compat/bionic/posix_bionic.cc'
SCANDIR_TRANSITION_SHA256 = '30447f7968242481142d2b4ac7cabff7231560210c1be8a5fd67393cef1b8758'
def preceding_scandir_source(current):
 folder=ROOT/'tools/net-tcp-priority/qualified'
 raw=(folder/'scandir-source.json').read_bytes()
 assert hashlib.sha256(raw).hexdigest()==SCANDIR_TRANSITION_SHA256,'scandir source transition changed'
 transition=json.loads(raw)
 assert set(transition)=={'baseline','baseline_policy_sha256','paths','before_sha256',
                         'after_sha256','patch_sha256'},'scandir transition schema changed'
 assert transition['baseline']==SCANDIR_BASE,'scandir baseline changed'
 paths=[SCANDIR_SOURCE]
 assert transition['paths']==paths,'scandir transition paths changed'
 assert sorted(transition['before_sha256'])==sorted(transition['after_sha256'])==paths,'scandir endpoint paths changed'
 policy=subprocess.check_output(['git','-C',str(ROOT),'show',SCANDIR_BASE+':tools/net-tcp-priority/test-selected-policy.py'])
 assert hashlib.sha256(policy).hexdigest()==transition['baseline_policy_sha256'],'scandir baseline policy changed'
 patch=(folder/'scandir-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==transition['patch_sha256'],'scandir transition patch changed'
 assert [line for line in patch.splitlines() if line.startswith(b'diff --git ')]==[
     ('diff --git a/'+SCANDIR_SOURCE+' b/'+SCANDIR_SOURCE).encode()],'scandir patch paths changed'
 assert hashlib.sha256(current).hexdigest()==transition['after_sha256'][SCANDIR_SOURCE],SCANDIR_SOURCE
 baseline=subprocess.check_output(['git','-C',str(ROOT),'show',SCANDIR_BASE+':'+SCANDIR_SOURCE])
 assert hashlib.sha256(baseline).hexdigest()==transition['before_sha256'][SCANDIR_SOURCE],SCANDIR_SOURCE
 assert current!=baseline,'empty scandir transition'
 with tempfile.TemporaryDirectory(prefix='gesher-scandir-qualification-') as temporary:
  work=Path(temporary)
  (work/SCANDIR_SOURCE).parent.mkdir(parents=True)
  (work/SCANDIR_SOURCE).write_bytes(current)
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert (work/SCANDIR_SOURCE).read_bytes()==baseline,'scandir reverse endpoint'
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert (work/SCANDIR_SOURCE).read_bytes()==current,'scandir forward endpoint'
 return baseline

# Reverse only the reviewed bounded memrchr word-search delta to the exact
# public main source before the unchanged OFD digest is checked. This admits
# no other POSIX byte and grants no supplier, controller or runtime approval.
MEMRCHR_BASE = '93c32f6f02521790eaf8936f2ff1fa1a650653f2'
MEMRCHR_SOURCE = 'compat/bionic/posix_bionic.cc'
MEMRCHR_TRANSITION_SHA256 = 'f814eed0ce7bee4fe49c0a36415e1fb4343db1930ef18115c5eb3693bd7f2c13'
def preceding_memrchr_source(qualification,read_source=live_source):
 folder=ROOT/'tools/net-tcp-priority/qualified'
 raw=(folder/'memrchr-source.json').read_bytes()
 assert hashlib.sha256(raw).hexdigest()==MEMRCHR_TRANSITION_SHA256,'memrchr source transition changed'
 transition=json.loads(raw)
 assert set(transition)=={'baseline','baseline_policy_sha256','paths','before_sha256',
                         'after_sha256','patch_sha256'},'memrchr transition schema changed'
 assert transition['baseline']==MEMRCHR_BASE,'memrchr baseline changed'
 paths=[MEMRCHR_SOURCE]
 assert transition['paths']==paths,'memrchr transition paths changed'
 assert sorted(transition['before_sha256'])==sorted(transition['after_sha256'])==paths,'memrchr endpoint paths changed'
 baseline_policy=subprocess.check_output(['git','-C',str(ROOT),'show',MEMRCHR_BASE+':tools/net-tcp-priority/test-selected-policy.py'])
 assert hashlib.sha256(baseline_policy).hexdigest()==transition['baseline_policy_sha256'],'memrchr baseline policy changed'
 patch=(folder/'memrchr-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==transition['patch_sha256'],'memrchr transition patch changed'
 assert [line for line in patch.splitlines() if line.startswith(b'diff --git ')]==[
     ('diff --git a/'+MEMRCHR_SOURCE+' b/'+MEMRCHR_SOURCE).encode()],'memrchr patch paths changed'
 current=preceding_scandir_source(read_source(MEMRCHR_SOURCE))
 assert hashlib.sha256(current).hexdigest()==transition['after_sha256'][MEMRCHR_SOURCE],MEMRCHR_SOURCE
 baseline=subprocess.check_output(['git','-C',str(ROOT),'show',MEMRCHR_BASE+':'+MEMRCHR_SOURCE])
 assert hashlib.sha256(baseline).hexdigest()==transition['before_sha256'][MEMRCHR_SOURCE],MEMRCHR_SOURCE
 assert transition['before_sha256'][MEMRCHR_SOURCE]==qualification['source_sha256'][MEMRCHR_SOURCE],MEMRCHR_SOURCE
 assert current!=baseline,'empty memrchr transition'
 with tempfile.TemporaryDirectory(prefix='gesher-memrchr-qualification-') as temporary:
  work=Path(temporary)
  (work/MEMRCHR_SOURCE).parent.mkdir(parents=True)
  (work/MEMRCHR_SOURCE).write_bytes(current)
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert (work/MEMRCHR_SOURCE).read_bytes()==baseline,'memrchr reverse endpoint'
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  assert (work/MEMRCHR_SOURCE).read_bytes()==current,'memrchr forward endpoint'
 return baseline

# Preserve the two donor receipts at their published source endpoints. Their
# patches precede the USB, Netlink and GLES property changes in this branch.
APPCONTAINER_IDENTITY_DONOR = '950dc2dbee2454d35e0f7265d50d146f41638264'
APPCONTAINER_IDENTITY_PATCH_SHA256 = 'b7a2c02e1561bd3096f9eeca4c04c37c25e77cf54c747cc222825083ddada2af'
APPCONTAINER_IDENTITY_SOURCE_SHA256 = {
 'compat/binder/binderd.cc': 'fbf2ca62efe037b081b7fa909806333fcea132cdbb8f165c2b69cb98d30a0aa7',
 'compat/elf/exec/exec_spawn.cc': 'feb52107f1d35105e7831a50466b9f24ae8c2b7396828dcaefa509b083476f33',
 'compat/posix/include/gesher/file_description.h': '94ce9ad7806d727977cc2c6f6df9fa289d1ea94b124404aed6c58fb7416a95d9',
 'compat/posix/include/gesher/system_properties.h': 'a414e1a7bca1f7974130c34d5103a0f94745b7aa02a9fd47e2de575fffaf8b8f',
 'compat/posix/posix.cmake': '8fe75797496a0561ac677ddf12b7b11e31e429daa2ec0379856ffe934392b756',
 'compat/posix/src/binder.cc': '10dbeec0a67daee5fc2363fa35db8282fbf80135b95805d2f05707bde3b2fd11',
 'compat/posix/src/binder_wire.h': '17a3474e3d2052b298db14262bb3431c54bce1069aac9c3c78e749721e406af5',
 'compat/posix/src/dirent.cc': '22e3bcf261e56a950fe7a1f3e719cfc5153505f2bd7a693eb6da82c71a4efcff',
 'compat/posix/src/errno.cc': 'f2d2e94807fec9abd2e3785994f388538b1dee9abc49a4b2e574cdd594f89238',
 'compat/posix/src/fd.cc': '4f7ab619d769b885e6f54be626567d4df8ec5a2739901b6beba8a2a5548305dc',
 'compat/posix/src/fsops.cc': '0e07ac571f75aa39273bf5674389e5ddfc0eadd536a772795c655d00c89d0dfa',
 'compat/posix/src/internal.h': '63d4a1383062aee4d6b96bbbbc0ec1c8858ae2af49d4ebabcaaee9b53d32033f',
 'compat/posix/src/ofd.cc': 'ce71c6ee99f9c417055eaa8d4eeb2196b74f9d3bdec3f5f46abd6d487f4ec6e1',
 'compat/posix/src/ofd_internal.h': 'ec2f2773ebeb78bbdf0fc1a450a3f6e17c565b811196c79f00520597dfc6a1f8',
 'compat/posix/src/path.cc': '78abebbd0f39d4b231906c9b14a03f0b30091a5dcad32e571d37365098a57647',
 'compat/posix/src/pipe_io.cc': '6df3e0ab8cae3afc9cf6cebc902c5950a7d612bea2745460cdd1447538e6e23b',
 'compat/posix/src/procfs.cc': '4c57dcd3dddd91c603c5a5344eb80ee6cbcc7191fce5d5890448f9b1763a42b5',
 'compat/posix/src/record_lock_descriptors.cc': '3289895673c4f63c98ae1c7434c89cce5700197c5b1401a4cc7f12cb79dcc819',
 'compat/posix/src/record_lock_descriptors.h': '915a45d8232f44ae07d1cfefe9d0b9c128253e6b16cc6f9fef1e185fd52d6e63',
 'compat/posix/src/record_lock_duplicate.cc': '563bc13b88b61ce80de4c3370849c843c316087039e98f12ec278ac0b49cfe4f',
 'compat/posix/src/record_lock_operation.cc': '5a61fd0f2c6dcd3204205cbbc35fb125efea21b155530816d9a0204e974d079f',
 'compat/posix/src/record_lock_operation.h': '90634149c714c0c0f8ac0e7e2af46a91f750981456ff90ac473243e8c661020a',
 'compat/posix/src/sqlite_path.cc': '793968fbd0e9d3002568a6da6f2ae2ec082bdb048d1d32ca3aa3f9496dab87fe',
 'compat/posix/src/stat.cc': '5a9fb602ff9bacb8fe8f612ec78c88e89a6da4a64d46db23cffb91cda66ece8e',
 'compat/posix/src/system.cc': 'e9a53915f7b79faecf03eb1d5a90ac93baccd6ae2c7101e64fae4613a09e40c3',
 'compat/posix/src/system_properties.cc': 'bb8b16bfd421ddef655bec8e933ea0196f18f1b697e59d13c6155c5c487b594a',
 'compat/posix/src/unistd.cc': '586933c9fbb8199caf5fe4fb562363a9397a8020b291b584ce82f2bbbcf8f0c4',
 'compat/posix/src/unix_rights.cc': '6092dca06e5b423d819151c1f1cad6150a3ddda667a71d99156491abbf2e4bb3',
 'tests/native/ofd_transfer_host_test.cc.in': 'd1f0a1ac8987d92e938e2b460d009108c79ee364bdc4bd79759a230f4c18891e',
 'tools/test_ofd_transfer_contracts.py': '6a0515f2f8ccad82c7cd8a1e1bceb25474af46db5d6ab2894441f7f644eaa545',
 'tools/test_revocable_file_contracts.py': '7a47c95932e421deecf31456aee22eb3358a713550eb57f6a39cb837c4d37019',
}

APPCONTAINER_PROPERTY_DONOR = 'c15e737a753e37c17298e1799b2f521444ccdfa3'
APPCONTAINER_PROPERTY_DONOR_BASE = 'd80a204d897ebda23575e30080f8760cbdcb0d56'
APPCONTAINER_PROPERTY_PATCH_SHA256 = '3da6f46e88a0845264a3ae077ec3a3654a24edece8b8880154961c17ddd6b1df'
APPCONTAINER_PROPERTY_SOURCE_SHA256 = {
 'compat/posix/include/gesher/system_properties.h': 'a414e1a7bca1f7974130c34d5103a0f94745b7aa02a9fd47e2de575fffaf8b8f',
 'compat/posix/src/system_properties.cc': '561b60eb0bf874438ba65fa8e3b9d5a8bcf3af67589c5a46105c0375fb1bb42b',
}

def appcontainer_donor_predecessor(name, revision, expected_patch, expected_sources):
 patch=(ROOT/'tools/net-tcp-priority/qualified'/f'appcontainer-{name}-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==expected_patch,('AppContainer donor transition changed',name)
 paths=list(expected_sources)
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==paths
 current={path:subprocess.check_output(['git','-C',str(ROOT),'show',revision+':'+path]) for path in paths}
 with tempfile.TemporaryDirectory(prefix='gesher-appcontainer-donor-qualification-') as temporary:
  work=Path(temporary)
  for path,source in current.items():
   assert hashlib.sha256(source).hexdigest()==expected_sources[path],path
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(source)
  apply=['git','-c','core.autocrlf=false','apply','--unidiff-zero']
  subprocess.run([*apply,'--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  previous={path:(work/path).read_bytes() for path in paths}
  subprocess.run([*apply,'-'],input=patch,cwd=work,check=True,capture_output=True)
  assert all((work/path).read_bytes()==current[path] for path in paths),('AppContainer donor forward endpoint',name)
 return previous

def check_appcontainer_donor_receipts():
 identity=appcontainer_donor_predecessor('identity',APPCONTAINER_IDENTITY_DONOR,APPCONTAINER_IDENTITY_PATCH_SHA256,APPCONTAINER_IDENTITY_SOURCE_SHA256)
 property_area=appcontainer_donor_predecessor('property',APPCONTAINER_PROPERTY_DONOR,APPCONTAINER_PROPERTY_PATCH_SHA256,APPCONTAINER_PROPERTY_SOURCE_SHA256)
 for path,source in property_area.items():
  expected=subprocess.check_output(['git','-C',str(ROOT),'show',APPCONTAINER_PROPERTY_DONOR_BASE+':'+path])
  assert source==expected and identity[path]==expected,('AppContainer donor property predecessor',path)

# Restore only the independently reviewed AppContainer integration delta before
# replaying the existing TCP/OFD, USB, and main feature qualifications. Both
# ends of each changed file are exact, and no runtime pass is granted here.
APPCONTAINER_INTEGRATION_BASE = 'ff2c1870053c9d0332e6abef07a25fc282d384b5'
APPCONTAINER_INTEGRATION_PATCH = 'tools/net-tcp-priority/qualified/appcontainer-integration-source.patch'
APPCONTAINER_INTEGRATION_QUALIFICATION = 'tools/net-tcp-priority/qualified/appcontainer-integration-source.json'

def preceding_appcontainer_integration_sources(receipt_sha256):
 raw=live_source(APPCONTAINER_INTEGRATION_QUALIFICATION)
 assert hashlib.sha256(raw).hexdigest()==receipt_sha256,'AppContainer integration qualification changed'
 qualification=json.loads(raw)
 assert set(qualification)=={'baseline','paths','previous_sha256','source_sha256','patch_sha256','native_runtime_approval'}
 assert qualification['baseline']==APPCONTAINER_INTEGRATION_BASE and qualification['native_runtime_approval'] is False
 paths=qualification['paths']
 assert paths and paths==sorted(qualification['source_sha256'])==sorted(qualification['previous_sha256'])
 assert all(p.startswith(('compat/','probes/','tests/','tools/')) and '..' not in Path(p).parts for p in paths)
 patch=live_source(APPCONTAINER_INTEGRATION_PATCH)
 assert hashlib.sha256(patch).hexdigest()==qualification['patch_sha256'],'AppContainer integration patch changed'
 assert [line for line in patch.splitlines() if line.startswith(b'diff --git ')]==[
     ('diff --git a/'+path+' b/'+path).encode() for path in paths]
 current={path:live_source(path) for path in paths}
 previous={path:subprocess.check_output(['git','-C',str(ROOT),'show',APPCONTAINER_INTEGRATION_BASE+':'+path]) for path in paths}
 apply=['git','-c','core.autocrlf=false','apply','--unidiff-zero']
 with tempfile.TemporaryDirectory(prefix='gesher-appcontainer-integration-qualification-') as temporary:
  work=Path(temporary)
  for path in paths:
   assert hashlib.sha256(current[path]).hexdigest()==qualification['source_sha256'][path],path
   assert hashlib.sha256(previous[path]).hexdigest()==qualification['previous_sha256'][path],('AppContainer integration baseline digest',path)
   assert current[path]!=previous[path],('empty AppContainer integration delta',path)
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(current[path])
  subprocess.run([*apply,'--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  for path in paths:
   assert (work/path).read_bytes()==previous[path],('AppContainer integration reverse endpoint',path)
  subprocess.run([*apply,'-'],input=patch,cwd=work,check=True,capture_output=True)
  for path in paths:
   assert (work/path).read_bytes()==current[path],('AppContainer integration forward endpoint',path)
 return previous

def appcontainer_integration_negative_controls(receipt_sha256):
 from unittest.mock import patch
 original=Path.read_bytes
 manifest=ROOT/APPCONTAINER_INTEGRATION_QUALIFICATION
 qualification=json.loads(original(manifest))
 path=qualification['paths'][0]
 target=ROOT/path
 altered=original(target)+b'\n// unqualified AppContainer integration bytes\n'
 cases=[({target:altered},receipt_sha256,path)]
 repinned={**qualification,'source_sha256':{**qualification['source_sha256'],path:hashlib.sha256(altered).hexdigest()}}
 cases.append(({target:altered},repinned,('AppContainer integration reverse endpoint',path)))
 patch_path=ROOT/APPCONTAINER_INTEGRATION_PATCH
 malformed=original(patch_path).replace(b'@@',b'@x',1)
 assert malformed!=original(patch_path)
 cases.append(({patch_path:malformed},receipt_sha256,'AppContainer integration patch changed'))
 cases.append(({}, {**qualification,'native_runtime_approval':True}, None))
 cases.append(({}, {**qualification,'baseline':'0'*40}, None))
 for overrides,receipt,expected in cases:
  if isinstance(receipt,dict):
   raw=(json.dumps(receipt,indent=2,sort_keys=True)+'\n').encode()
   overrides[manifest]=raw
   receipt=hashlib.sha256(raw).hexdigest()
  def read(path):return overrides[path] if path in overrides else original(path)
  with patch.object(Path,'read_bytes',read):
   try:preceding_appcontainer_integration_sources(receipt)
   except AssertionError as error:assert (error.args[0] if error.args else None)==expected,error
   else:raise AssertionError('unqualified AppContainer transition accepted')
 print('TCP_APPCONTAINER_INTEGRATION_NEGATIVE_CONTROLS_PASS cases='+str(len(cases))+' native_processes_started=0')

def preceding_ofd_sources(read_source=live_source):
 stdio_main=preceding_stdio_feature_sources(read_source)
 historical=preceding_stdio_line_sources({path:subprocess.check_output(['git','-C',str(ROOT),'show',COMPOSED_STDIO_PRIOR_FEATURE+':'+path]) for path in COMPOSED_STDIO_PATHS})
 historical_scan=preceding_stdio_scan_source(subprocess.check_output(['git','-C',str(ROOT),'show',COMPOSED_SCAN_PRIOR_FEATURE+':'+COMPOSED_SCAN_PATHS[0]]))
 folder=ROOT/'tools/net-tcp-priority/qualified'
 raw=(folder/'ofd-stream-source.json').read_bytes()
 assert hashlib.sha256(raw).hexdigest()==OFD_QUALIFICATION_SHA256,'OFD source qualification changed'
 qualification=json.loads(raw)
 patch=(folder/'ofd-stream-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==qualification['patch_sha256'],'OFD transition patch changed'
 paths=['compat/posix/src/fd.cc','compat/posix/src/socket.cc','compat/posix/src/socket_internal.h',
        'compat/posix/src/system.cc','compat/posix/src/winsock.cc']
 assert qualification['paths']==paths
 assert [line.decode().split(' b/',1)[1] for line in patch.splitlines() if line.startswith(b'diff --git ')]==paths
 usb_block=preceding_usb_block_sources(folder,read_source)
 netlink=preceding_netlink_sources(usb_block,read_source)
 before_features={**usb_block,**netlink}  # The overlapping Netlink endpoints are the older bytes.
 memfd_lease=preceding_memfd_lease_sources(usb_block,read_source)
 shared_input={path:stdio_main[path] if path in stdio_main else read_source(path) for path in SHARED_STDIO_PATHS}
 shared_stdio=preceding_shared_stdio_sources(qualification,shared_input)
 assert shared_stdio[COMPOSED_SCAN_PATHS[0]]==historical_scan,'historical scanner and composed shared-stdio normalization disagree'
 for path,previous in historical.items():
  current=shared_stdio[path] if path in shared_stdio else stdio_main[path]
  if path=='compat/posix/src/misc.cc':
   # Main added real dumpability/ptrace protections after the historical receipt.
   # Keep both complete immutable endpoints; the reviewed line delta is still
   # reversed separately above and every current OFD source digest is unchanged.
   expected=subprocess.check_output(['git','-C',str(ROOT),'show',COMPOSED_STDIO_BASELINE+':'+path])
   assert current==expected,('current main misc endpoint changed',path)
  else:
   assert current==previous,('historical stdio and current-main normalization disagree',path)
 storage_policy=preceding_storage_policy_sources(before_features,read_source)
 assert memfd_lease.keys().isdisjoint(shared_stdio.keys()|stdio_main.keys()),'source qualifications overlap'
 assert storage_policy.keys().isdisjoint(memfd_lease.keys()|shared_stdio.keys()|stdio_main.keys()),'source qualifications overlap'
 preceding={**before_features,**memfd_lease,**stdio_main,**shared_stdio,**storage_policy}
 for path,digest in qualification['source_sha256'].items():
  actual=preceding[path] if path in preceding else read_source(path)
  if path=='compat/binder/binderd.cc':actual=preceding_composed_binder_output_source(actual)
  if path=='compat/posix/src/system_properties.cc':
   header='compat/posix/include/gesher/system_properties.h'
   actual=preceding_logging_property_source(actual,read_source(header))
  if path=='compat/bionic/shim.cc':actual=preceding_memmem_source(actual)
  if path==MEMRCHR_SOURCE:actual=preceding_memrchr_source(qualification,read_source)
  assert hashlib.sha256(actual).hexdigest()==digest,path
 with tempfile.TemporaryDirectory(prefix='gesher-ofd-qualification-') as temporary:
  work=Path(temporary)
  for path in paths:
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(preceding.get(path,read_source(path)))
  reverse=['git','-c','core.autocrlf=false','apply','--reverse','--unidiff-zero']
  subprocess.run([*reverse,'--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run([*reverse,'-'],input=patch,cwd=work,check=True,capture_output=True)
  previous={path:(work/path).read_bytes() for path in paths}
 def function(source,marker):
  assert source.count(marker)==1,marker
  begin=source.index(marker);start=source.index(b'{',begin);depth=1;end=start+1
  while depth:
   assert end<len(source),marker
   depth+=(source[end:end+1]==b'{')-(source[end:end+1]==b'}');end+=1
  return source[begin:end]
 # Independently retain socket ownership and actual native poll core bytes.
 for path,markers in {
   'compat/posix/src/fd.cc':[b'bool Remove(',b'void Insert(',b'bool ReplaceSocket(',b'bool LookupSocket(',b'static int PollFdsImpl('],
   'compat/posix/src/system.cc':[b'bool SocketIo('],
 }.items():
  current=netlink[path] if path in netlink else live_source(path)
  for marker in markers:
   actual=function(current,marker);expected=function(previous[path],marker)
   if marker==b'static int PollFdsImpl(':
    for before,after in PIPE_POLL_ERROR_REPLACEMENTS:
     assert actual.count(after)==expected.count(before)==1,(path,marker)
     actual=actual.replace(after,before,1)
   assert actual==expected,(path,marker)
 return previous
# Guest fork (ADR 0092) registers two fd.cc locks and one winsock.cc lock in
# its fork set. Each file's one exact addition at its end, and nothing else, is
# removed before the reviewed hunks and every byte check below run on the result.
FORK_LOCK_ADDITIONS={
 'compat/posix/src/fd.cc':(b'''
// The fork set (gesher/fork.h); absent where a host test compiles this file alone.
#if __has_include(<gesher/fork.h>)
#include <gesher/fork.h>
#endif
#ifdef GESHER_FORK_LOCK
// fork (gesher/fork.h): the socket table and descriptor waiters.
GESHER_FORK_LOCK(gesher::kForkOrderDescriptors, std::mutex, gesher::table_mutex);
GESHER_FORK_LOCK(gesher::kForkOrderLeaf, std::mutex, waiters_mutex);
#endif
''',),
 'compat/posix/src/winsock.cc':(b'''
// The fork set (gesher/fork.h); absent where a host test compiles this file alone.
#if __has_include(<gesher/fork.h>)
#include <gesher/fork.h>
#endif
#ifdef GESHER_FORK_LOCK
// fork (gesher/fork.h): the local IPv4 address cache recvmsg's IP_PKTINFO reads.
GESHER_FORK_LOCK(gesher::kForkOrderLeaf, std::mutex, gesher::ws::LocalAddressCache().mutex);
#endif
''',),
}
# AF_UNIX credentials and descriptor passing (ADR 0088) and memfd seals change
# these selected sources after REVIEWED_CHECKPOINT, a commit on main. The
# pinned REVIEWED_PATCH lists each changed line as an exact hunk at its line in
# the checkpoint's file: every current file must be its checkpoint bytes with
# exactly those hunks applied, and the derivations below still verify the
# checkpoint's bytes. Any other change fails here as before.
REVIEWED_CHECKPOINT='7a45c2468c270bfe68ee5f854aa9d4feaf2208b3'
REVIEWED_PATCH='tools/net-tcp-priority/reviewed-adr0088-memfd-seals.json'
# ADR 0097 replaces system.cc's ENOSYS mkfifo stub with a relocation comment.
# The reviewed receipt adds exactly that hunk; every byte check below remains.
REVIEWED_PATCH_SHA256='ee4583db58b83515430b5a4333cb95d3ef037ddc5731f5ad0f7958ba858a8e55'
def reviewed(path,actual):
 raw=(ROOT/REVIEWED_PATCH).read_bytes()
 assert hashlib.sha256(raw).hexdigest()==REVIEWED_PATCH_SHA256,REVIEWED_PATCH
 patch=json.loads(raw)
 assert patch['checkpoint']==REVIEWED_CHECKPOINT and sorted(patch['hunks'])==sorted(REVIEWED_PATHS),REVIEWED_PATCH
 base=subprocess.check_output(['git','-C',str(ROOT),'show',REVIEWED_CHECKPOINT+':'+path])
 lines=base.decode('utf-8').splitlines(keepends=True);rebuilt=[];cursor=0
 for start,before,after in patch['hunks'][path]:
  assert start>=cursor and lines[start:start+len(before)]==before and before!=after,path
  rebuilt+=lines[cursor:start]+after;cursor=start+len(before)
 assert ''.join(rebuilt+lines[cursor:]).encode('utf-8')==actual,path
 return base
# The TCP jobs restore the authenticated r27c publisher archive from the
# cache entry native2 and the fonts environment job already keep, instead of
# a second copy of the same 663987688-byte zip under its own path and key.
# prepare-ndk.py still checks the publisher sha1 and size, downloads on a
# miss and extracts freshly on every use; only these exact lines may differ.
NDK_ARCHIVE_SHARING={
 '.github/workflows/net-tcp-coherent-component.yml':(
  b'          path: tcp-ndk-archive\n          key: tcp-ndk-r27c-090e8083a715fdb1a3e402d0763c388abb03fb4e\n',
  b'          path: dns-ndk.zip\n          key: combined-ndk-r27c-090e8083a715fdb1a3e402d0763c388abb03fb4e\n'),
 'tools/net-tcp-priority/prepare-ndk.py':(
  b"archive = Path('tcp-ndk-archive/android-ndk-r27c-linux.zip')\narchive.parent.mkdir(exist_ok=True)\n",
  b"# Shares native2's publisher-archive cache entry (same path and key), so one copy is kept.\narchive = Path('dns-ndk.zip')\n"),
}
# The independently reviewed TSF repair gives output-size queries their own
# metadata-only file objects. Reverse only its exact two-file transition before
# every original controller/fixture comparison below. The qualified TCP copy,
# suppliers, deadlines and approval receipts remain unchanged; this is source
# identity qualification, not proof of Windows execution or runtime approval.
TSF_METADATA_BASE='3b2eaca95b5ddd9cee3c3db4e430460e512eb355'
TSF_METADATA_FEATURE_HEAD='8d955bd7b6ec85b84fa31f7df685197ddb963276'
TSF_METADATA_QUALIFICATION_SHA256='b6f40f75fa20ebfd9e47473c2cd2a2d5dfe697dd4b01e50d2c04f925545d6e22'
TSF_METADATA_PATHS=('tools/windows4/TsfJobSupervisor.cs','tools/windows4/test-tsf-job-supervisor.ps1')
TSF_METADATA_ADDITIONAL_SOURCE='tools/windows4/TsfOutputMetadataChecks.cs'
def preceding_tsf_metadata_sources(read_source=live_source):
 folder=ROOT/'tools/net-tcp-priority/qualified'
 raw=(folder/'tsf-output-metadata-source.json').read_bytes()
 assert hashlib.sha256(raw).hexdigest()==TSF_METADATA_QUALIFICATION_SHA256,'TSF metadata qualification changed'
 transition=json.loads(raw)
 assert set(transition)=={'baseline','feature_head','baseline_policy_sha256','paths',
                         'before_sha256','after_sha256','additional_sources','patch_sha256'},'TSF metadata schema changed'
 assert transition['baseline']==TSF_METADATA_BASE,'TSF metadata baseline changed'
 assert transition['feature_head']==TSF_METADATA_FEATURE_HEAD,'TSF metadata feature checkpoint changed'
 paths=list(TSF_METADATA_PATHS)
 assert transition['paths']==paths,'TSF metadata paths changed'
 assert sorted(transition['before_sha256'])==sorted(transition['after_sha256'])==paths,'TSF metadata endpoint paths changed'
 assert list(transition['additional_sources'])==[TSF_METADATA_ADDITIONAL_SOURCE],'TSF metadata additional paths changed'
 baseline_policy=subprocess.check_output(['git','-C',str(ROOT),'show',TSF_METADATA_BASE+':tools/net-tcp-priority/test-selected-policy.py'])
 assert hashlib.sha256(baseline_policy).hexdigest()==transition['baseline_policy_sha256'],'TSF metadata baseline policy changed'
 additional=read_source(TSF_METADATA_ADDITIONAL_SOURCE)
 assert hashlib.sha256(additional).hexdigest()==transition['additional_sources'][TSF_METADATA_ADDITIONAL_SOURCE],TSF_METADATA_ADDITIONAL_SOURCE
 published=subprocess.check_output(['git','-C',str(ROOT),'show',TSF_METADATA_FEATURE_HEAD+':'+TSF_METADATA_ADDITIONAL_SOURCE])
 assert additional==published,'TSF metadata additional source endpoint'
 patch=(folder/'tsf-output-metadata-source.patch').read_bytes()
 assert hashlib.sha256(patch).hexdigest()==transition['patch_sha256'],'TSF metadata patch changed'
 assert [line for line in patch.splitlines() if line.startswith(b'diff --git ')]==[
     ('diff --git a/'+path+' b/'+path).encode() for path in paths],'TSF metadata patch paths changed'
 current={};baseline={}
 for path in paths:
  current[path]=read_source(path)
  assert hashlib.sha256(current[path]).hexdigest()==transition['after_sha256'][path],path
  baseline[path]=subprocess.check_output(['git','-C',str(ROOT),'show',TSF_METADATA_BASE+':'+path])
  assert hashlib.sha256(baseline[path]).hexdigest()==transition['before_sha256'][path],path
  assert current[path]!=baseline[path],('empty TSF metadata transition',path)
 with tempfile.TemporaryDirectory(prefix='gesher-tsf-metadata-qualification-') as temporary:
  work=Path(temporary)
  for path in paths:
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(current[path])
  subprocess.run(['git','apply','--reverse','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','-'],input=patch,cwd=work,check=True,capture_output=True)
  for path in paths:
   assert (work/path).read_bytes()==baseline[path],('TSF metadata reverse endpoint',path)
  subprocess.run(['git','apply','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','-'],input=patch,cwd=work,check=True,capture_output=True)
  for path in paths:
   assert (work/path).read_bytes()==current[path],('TSF metadata forward endpoint',path)
 return baseline

REVIEWED_PATHS=('compat/posix/src/fd.cc','compat/posix/src/socket.cc','compat/posix/src/socket_internal.h','compat/posix/src/system.cc','compat/posix/src/winsock.cc')
def old(path):return subprocess.check_output(['git','-C',str(ROOT),'show',BASE+':'+path])
def check(read_source=live_source):
 preceding=preceding_ofd_sources(read_source)
 preceding.update(preceding_tsf_metadata_sources(read_source))
 assert hashlib.sha256(read_source(OWNER_THREAD_DIAGNOSTIC_PATH)).hexdigest()==OWNER_THREAD_DIAGNOSTIC_SHA256,OWNER_THREAD_DIAGNOSTIC_PATH
 assert hashlib.sha256(read_source(FD_POLL_SCRATCH_HEADER)).hexdigest()==FD_POLL_SCRATCH_SHA256,FD_POLL_SCRATCH_HEADER
 paths=subprocess.check_output(['git','-C',str(ROOT),'diff','1745e99d7f2849ba81784545e07d60cb2b7800c3^','1745e99d7f2849ba81784545e07d60cb2b7800c3','--name-only'],text=True).splitlines()
 paths+=['.github/workflows/net-tcp-coherent-component.yml','tests/net-tcp-priority/contract.cc','tests/net-tcp-priority/native_udp_control.cc','tests/native2/net_udp_trunc.cpp','tests/native2/net_udp_fault_observer.c','tools/net-tcp-priority/qualified/TsfJobSupervisor.cs','tools/windows4/TsfJobSupervisor.cs','tools/windows4/TsfJobWorker.cs','tools/windows4/test-tsf-job-supervisor.ps1','tools/ci/receipt-diagnostics.ps1','tools/net-tcp-priority/import-policy.json','tools/net-tcp-priority/import_policy.py','tools/net-tcp-priority/build-guests.py','tools/net-tcp-priority/prepare-ndk.py']
 for path in paths:
  epoch=(FD_BASE if path=='compat/posix/src/fd.cc' else
         SOCKET_BASE if path=='compat/posix/src/socket.cc' else
         WINSOCK_BASE if path=='compat/posix/src/winsock.cc' else
         CONTROL_BASE if path=='tools/windows4/test-tsf-job-supervisor.ps1' else BASE)
  expected=subprocess.check_output(['git','-C',str(ROOT),'show',epoch+':'+path])
  actual=preceding.get(path,read_source(path))
  for addition in FORK_LOCK_ADDITIONS.get(path,()):
   assert actual.count(addition)==1 and actual.endswith(addition),path
   actual=actual[:-len(addition)]
  if path=='compat/posix/src/winsock.cc':
   assert actual.count(TCP_PRIORITY_WAKE_AFTER)==1 and TCP_PRIORITY_WAKE_BEFORE not in actual,path
   actual=actual.replace(TCP_PRIORITY_WAKE_AFTER,TCP_PRIORITY_WAKE_BEFORE,1)
  if path in NDK_ARCHIVE_SHARING:
   before,after=NDK_ARCHIVE_SHARING[path]
   assert actual.count(after)==1 and before not in actual,path
   actual=actual.replace(after,before,1)
  if path in REVIEWED_PATHS:actual=reviewed(path,actual)
  if path=='compat/posix/src/winsock.cc':
   assert actual==expected and hashlib.sha256(actual).hexdigest()==WINSOCK_SHA256,path
   original=old(path)
   start=b"namespace {\n// Winsock's WSAPOLLFD"
   end=b'// --- names --------------------------------------------------------------------'
   assert expected.count(start)==original.count(start)==1 and expected.count(end)==original.count(end)==1,path
   assert expected[:expected.index(start)]==original[:original.index(start)],path
   assert expected[expected.index(end):]==original[original.index(end):],path
  elif path=='compat/posix/src/fd.cc':
   assert expected.count(FD_DEVICE_INSERTION)==actual.count(FD_DEVICE_INSERTION)==1,path
   for addition in (FD_DEVICE_LOOKUP,FD_PRECEDING_STREAM_DEVICE_RETIREMENT):
    assert actual.count(addition)==1 and addition not in expected,path
   additions=FD_DEVICE_LOOKUP+FD_PRECEDING_STREAM_DEVICE_RETIREMENT
   expected=expected.replace(FD_DEVICE_INSERTION,additions+FD_DEVICE_INSERTION,1)
   for before,after in FD_POLL_SCRATCH_REPLACEMENTS:
    assert expected.count(before)==actual.count(after)==1,path
    assert after not in expected and before not in actual,path
    expected=expected.replace(before,after,1)
   assert actual==expected,path
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
 assert before.count(OWNER_THREAD_CONTROLLER_BEFORE)==after.count(OWNER_THREAD_CONTROLLER_AFTER)==1
 before=before.replace(OWNER_THREAD_CONTROLLER_BEFORE,OWNER_THREAD_CONTROLLER_AFTER,1)
 marker='function RequireDiagnosticRetirement';assert after[after.index(marker):]==before[before.index(marker):],'Actual controller/guarded-copy/census/cancel/HOST/bothABI execution gates changed'
 raw=old('.github/workflows/net-tcp-coherent-component.yml').decode()
 expected=raw.replace('name: net-tcp-coherent-component','name: net-tcp-selected-component',1).replace('wip/tcp-coherent-port','wip/tcp-minimum-selection').replace('.github/workflows/net-tcp-coherent-component.yml','.github/workflows/net-tcp-selected-component.yml').replace('net-tcp-coherent-component-${{ github.ref }}','net-tcp-selected-component-${{ github.ref }}').replace('supplier: [canonical, retirement]','supplier: [canonical, selected-minimum]').replace('else prefix=tcp-private-supplier','else prefix=tcp-selected-supplier').replace('separately patched diagnostic compiler','intentionally selected two-patch original-profile candidate').replace('# canonical supplier separate from the minimal-retirement diagnostic supplier.','# canonical supplier separate from the intentionally selected minimum candidate.')
 expected=expected.replace('      - uses: actions/checkout@v4\n      - uses: ./.github/actions/setup-build','      - uses: actions/checkout@v4\n        with:\n          fetch-depth: 0\n      - name: Verify exact source and two-supplier preservation before native compilation\n        run: |\n          python3 tools/net-tcp-priority/test-selected-policy.py\n          python3 tools/net-tcp-priority/test-selected-supplier.py\n          python3 tools/host-pthread-authority/minimum_receipt.py selftest\n      - uses: ./.github/actions/setup-build',1)
 before,after=(s.decode() for s in NDK_ARCHIVE_SHARING['.github/workflows/net-tcp-coherent-component.yml']);assert expected.count(before)==1;expected=expected.replace(before,after,1)
 actual=(ROOT/'.github/workflows/net-tcp-selected-component.yml').read_text(encoding='utf-8');assert actual==expected,'Supplier-independent DAG/commands changed'
 pin=json.loads((ROOT/'tools/host-pthread-authority/minimum-inputs.json').read_text());assert pin['role']=='gesher-winpthreads-selected-minimum-v1' and len(pin['source_sha256'])==len(pin['output_source_sha256'])==12
 assert pin['ordered_patch_sha256']=={'minimum-detached-retirement.patch':'ebc5aac990c6ec075431d0c25a9b74ecadc0940da1bdad9503f358d4fe3431ad','minimum-noattach-join.patch':'c4f47903af720542a584fa743932316d86c45683da70a4c8620d7a71d3eb7f96'}
 for name,digest in pin['ordered_patch_sha256'].items():assert hashlib.sha256((ROOT/'tools/host-pthread-authority'/name).read_bytes()).hexdigest()==digest
 for path in ['tools/net-tcp-priority/manifest.py','tools/host-pthread-authority/minimum_receipt.py']:ast.parse((ROOT/path).read_text(encoding='utf-8'))
 print('TCP_SELECTED_POLICY_PASS exact_OFD_stdio_pipe_transition exact_property_revision_transition exact_gles_property_transition exact_memfd_lease_transition exact_storage_policy_transition reviewed_pipe_control_errors reviewed_process_lifetime_socket_registry reviewed_outer_poll_scratch_exact reviewed_priority_wake_confirmation_only original_socket_ownership_and_native_poll=EXACT original_product3 reviewed_nested_epoll_fd reviewed_device_lookup_and_stream_retirement_only reviewed_sendmsg_control reviewed_statvfs_affinity_file_advice_allocation_writev_and_memfd_only reviewed_fcntl_argument_selection_only canonical_Recv originalworkflow other_fixture_assertions_deadlines=EXACT reviewed_select_and_calibrated_owner_census controller_execution_tail original_import_NDK_build_roles=EXACT new_two_supplier_DAG=BOUNDED source12_patch2_exact native_processes_started=0')

def usb_main513_negative_controls():
 # These substitutions touch only in-memory reads, never the worktree. Even
 # repinning a changed receipt/patch cannot move either historical endpoint.
 from unittest.mock import patch
 original=Path.read_bytes
 qualification=json.loads(original(ROOT/USB_MAIN513_QUALIFICATION))
 delta=original(ROOT/USB_MAIN513_PATCH)
 checked=0
 cases=[]
 for field in ('base','commit','native_runtime_approval','path_bases','before_sha256','after_sha256'):
  changed=json.loads(json.dumps(qualification))
  if field in ('path_bases','before_sha256','after_sha256'):
   changed[field][USB_MAIN513_PATHS[0]]='0'*len(changed[field][USB_MAIN513_PATHS[0]])
  elif field=='native_runtime_approval':changed[field]=True
  else:changed[field]='0'*40
  receipt=(json.dumps(changed,indent=2,sort_keys=True)+'\n').encode()
  cases.append((field,receipt,None,True))
 cases.append(('unrepinned receipt',original(ROOT/USB_MAIN513_QUALIFICATION)+b'\n',None,False))
 for repin in (False,True):
  bad=delta.replace(b'@@',b'@x',1)
  assert bad!=delta
  changed=json.loads(json.dumps(qualification))
  if repin:changed['patch_sha256']=hashlib.sha256(bad).hexdigest()
  receipt=(json.dumps(changed,indent=2,sort_keys=True)+'\n').encode()
  cases.append(('patch syntax repin='+str(repin),receipt,bad,repin))
 # A valid patch with a changed transition must also fail after updating its
 # hash: the unchanged authoritative incoming bytes will not reverse to it.
 marker=b'+  std::atomic<signed char> procfs_file{-1};'
 assert delta.count(marker)==1
 bad=delta.replace(marker,b'+  std::atomic<signed char> procfs_file{0};',1)
 changed=json.loads(json.dumps(qualification));changed['patch_sha256']=hashlib.sha256(bad).hexdigest()
 cases.append(('semantic patch repin',(json.dumps(changed,indent=2,sort_keys=True)+'\n').encode(),bad,True))
 for name,receipt,changed_patch,repin in cases:
  def reads(path):
   if path==ROOT/USB_MAIN513_QUALIFICATION:return receipt
   if changed_patch is not None and path==ROOT/USB_MAIN513_PATCH:return changed_patch
   return original(path)
  pins={'USB_MAIN513_QUALIFICATION_SHA256':hashlib.sha256(receipt).hexdigest()} if repin else {}
  rejected=None
  with patch.object(Path,'read_bytes',reads),patch.dict(globals(),pins):
   try:qualified_usb_main513_endpoints()
   except (AssertionError,subprocess.CalledProcessError) as error:rejected=error
  assert rejected is not None,'USB main513 invalid transition accepted: '+name
  if repin:
   assert 'qualification changed' not in str(rejected) and 'patch changed' not in str(rejected),name
  checked+=1
 print('TCP_USB_MAIN513_TRANSITION_NEGATIVE_CONTROLS_PASS cases='+str(checked)+' native_processes_started=0')


def storage_policy_negative_controls():
 usb_preceding=preceding_usb_block_sources(ROOT/'tools/net-tcp-priority/qualified')
 # Isolated read substitution only: no source files are changed or executed.
 # Refreshing this transition's after hash cannot admit an extra byte outside
 # its exact patch, because the reverse endpoint is still the historical main.
 from unittest.mock import patch
 original=Path.read_bytes
 qualification=json.loads(original(ROOT/STORAGE_POLICY_QUALIFICATION))
 before_features={**usb_preceding,**preceding_netlink_sources(usb_preceding)}
 checked=repinned=0
 for name in STORAGE_POLICY_PATHS+STORAGE_POLICY_HELPERS+[STORAGE_POLICY_PATCH]:
  target=ROOT/name
  source=before_features.get(name,original(target))
  data=source.replace(b'@@',b'@x',1) if name==STORAGE_POLICY_PATCH else source+b'\n'
  assert data!=source,name
  for repin in (False,True) if name in STORAGE_POLICY_PATHS or name==STORAGE_POLICY_PATCH else (False,):
   pins={}
   receipt=None
   if repin:
    updated=json.loads(json.dumps(qualification))
    if name==STORAGE_POLICY_PATCH:updated['patch_sha256']=hashlib.sha256(data).hexdigest()
    else:updated['source_sha256'][name]=hashlib.sha256(data).hexdigest()
    receipt=(json.dumps(updated,indent=2)+'\n').encode()
    pins['STORAGE_POLICY_QUALIFICATION_SHA256']=hashlib.sha256(receipt).hexdigest()
    repinned+=1
   def reads(path):
    if path==target:return data
    if receipt is not None and path==ROOT/STORAGE_POLICY_QUALIFICATION:return receipt
    return original(path)
   stage=dict(before_features)
   if name in stage:stage[name]=data
   rejected=None
   with patch.object(Path,'read_bytes',reads),patch.dict(globals(),pins):
    try:preceding_storage_policy_sources(stage)
    except (AssertionError,subprocess.CalledProcessError) as error:rejected=error
   assert rejected is not None,'unqualified storage source accepted: '+name+' repin='+str(repin)
   if repin:
    message=str(rejected)
    assert 'qualification changed' not in message and 'transition patch changed' not in message and message!=name,(name,message)
   checked+=1
 for field in ('baseline_policy_sha256','previous_sha256'):
  updated=json.loads(json.dumps(qualification))
  if field=='previous_sha256':updated[field][STORAGE_POLICY_PATHS[0]]='0'*64
  else:updated[field]='0'*64
  receipt=(json.dumps(updated,indent=2)+'\n').encode()
  def reads(path):return receipt if path==ROOT/STORAGE_POLICY_QUALIFICATION else original(path)
  rejected=None
  with patch.object(Path,'read_bytes',reads),patch.dict(globals(),{'STORAGE_POLICY_QUALIFICATION_SHA256':hashlib.sha256(receipt).hexdigest()}):
   try:preceding_storage_policy_sources(before_features)
   except (AssertionError,subprocess.CalledProcessError) as error:rejected=error
  assert rejected is not None,'storage historical endpoint changed: '+field
  checked+=1
 print('TCP_STORAGE_POLICY_TRANSITION_NEGATIVE_CONTROLS_PASS cases='+str(checked)+' repinned_transition_controls='+str(repinned)+' native_processes_started=0')

STDIO_LINE_NEGATIVE_CASES=[
  # Reject changes inside each admitted line-input source, including the new
  # helper and ABI entry, and reject qualification/transition drift itself.
  ('compat/bionic/stdio_bionic.cc',b'line != nullptr && n != nullptr && Orient(fp, -1)',b'Orient(fp, -1)'),
  ('compat/posix/include/gesher/stdio.h',b'bool unlocked = false);',b'bool unlocked = true);'),
  ('compat/posix/src/misc.cc',b'gesher::stdio::ReadDelimited(lineptr, n, delim, stream)',b'gesher::stdio::ReadDelimited(lineptr, n, 0, stream)'),
  ('compat/posix/src/stdio_output.cc',b'"gesher_stdio_delimited_v1"',b'"gesher_stdio_delimited_v2"'),
  ('compat/posix/src/stdio_stream_buffer.h',b'static_cast<unsigned char>(delimiter), span.size)',b'0, span.size)'),
  ('tools/net-tcp-priority/qualified/stdio-line-source.json',b'"native_runtime_approval": false',b'"native_runtime_approval": true'),
  ('tools/net-tcp-priority/qualified/stdio-line-source.patch',b'@@',b'@x'),
  ('tools/net-tcp-priority/qualified/stdio-line-shared-source.patch',b'@@',b'@x'),
]
STDIO_SCAN_NEGATIVE_CASES=[
  ('compat/posix/src/stdio_output.cc',b'thread_local State* scanning_stream = nullptr;',b'State* scanning_stream = nullptr;'),
  ('compat/posix/src/stdio_output.cc',b'scanning_stream->stream == stream && scanning_stream->live',b'true'),
  ('compat/posix/src/stdio_output.cc',b'scanning_stream = static_cast<ScanScope*>(raw)->previous_;',b'scanning_stream = nullptr;'),
  ('compat/posix/src/stdio_output.cc',b'  int fd = -1;',b'  int fd = 0;'),
  ('tools/net-tcp-priority/qualified/stdio-scan-source.json',b'"native_runtime_approval": false',b'"native_runtime_approval": true'),
  ('tools/net-tcp-priority/qualified/stdio-scan-source.patch',b'@@',b'@x'),
  ('tools/net-tcp-priority/qualified/stdio-scan-shared-source.patch',b'@@',b'@x'),
]

SNPRINTF_BOUNDS_NEGATIVE_CASES=[
  (SNPRINTF_BOUNDS_SOURCE,b'return bytes.size() + omitted;',b'return bytes.size();'),
  (SNPRINTF_BOUNDS_SOURCE,b'size_t kept = count < Remaining() ? count : Remaining();',b'size_t kept = count;'),
  (SNPRINTF_BOUNDS_SOURCE,b'out->bytes.resize(offset + kept);',b'out->bytes.resize(offset);'),
  (SNPRINTF_BOUNDS_SOURCE,b'if (!valid) return -1;\n  if (n != 0) {',b'if (!valid) { if (n) buf[0] = 0; return -1; }\n  if (n != 0) {'),
  # An unrelated unbounded formatter remains protected by the original digest.
  (SNPRINTF_BOUNDS_SOURCE,b'std::memcpy(buf, s.c_str(), s.size() + 1);\n  return static_cast<int>(s.size());\n}\nint BIONIC_SYSV gesher_bionic_vfprintf',b'std::memcpy(buf, s.c_str(), s.size());\n  return static_cast<int>(s.size());\n}\nint BIONIC_SYSV gesher_bionic_vfprintf'),
  ('tools/net-tcp-priority/qualified/snprintf-bounds-source.patch',b'@@',b'@x'),
]

def transition_negative_controls():
 from unittest.mock import patch
 original=Path.read_bytes
 usb_preceding=preceding_usb_block_sources(ROOT/'tools/net-tcp-priority/qualified')
 cases=[
  ('compat/posix/src/fd.cc',b'--state->description->descriptor_refs == 0',b'--state->description->descriptor_refs == 1'),
  ('compat/posix/src/fd.cc',b'return TransferPipe(snapshot, bytes, count, false);',b'return 0;'),
  ('compat/posix/src/system.cc',b'gesher::ws::RecvOwned(state.socket,',b'gesher::ws::SendOwned(state.socket,'),
  ('compat/posix/src/pipe_io.cc',b'if (!count) return 0;',b'if (!count) return 1;'),
  ('compat/posix/src/pipe_io.cc',b'TokenIsAppContainer',b'TokenElevation'),
  ('compat/posix/src/ofd.cc',b'InterlockedCompareExchange(&shared->flags,',b'InterlockedExchange(&shared->flags,'),
  ('compat/elf/exec/shell_execute.inc',b'gesher::ReadPipeDescriptionInterruptible(description, value, 1, interrupted)',b'gesher::ReadFileDescription(description, value, 1)'),
  ('tools/net-tcp-priority/qualified/ofd-stream-source.patch',b'@@',b'@x'),
  ('tools/net-tcp-priority/qualified/usb-block-source.patch',b'@@',b'@x'),
  ('compat/posix/include/gesher/usb_block.h',b'AttachBroker',b'AttachUncheckedBroker'),
  ('compat/elf/exec/exec_spawn.cc',b'"basename"',b'"not-basename"'),
  ('compat/posix/src/fd.cc',b'if (result < 0) return -1;',b'if (result < 0) return 0;'),
  ('compat/posix/src/fd.cc',b'SocketRegistry& socket_registry = *new (socket_registry_storage) SocketRegistry;',b'SocketRegistry socket_registry;'),
  ('compat/posix/src/stdio_output.cc',b'if (locked_stream_) _lock_file(locked_stream_);',b'if (locked_stream_) {}'),
  ('compat/posix/src/socket.cc',b'if (flags & MSG_CMSG_CLOEXEC) result |= ws::kMsgCmsgCloexec;',b'if (flags & MSG_CMSG_CLOEXEC) result |= 0;'),
  ('compat/posix/src/socket.cc',b'gesher::ClaimRights(token, rights, (flags & MSG_CMSG_CLOEXEC) != 0);',b'gesher::ClaimRights(token, rights, true);'),
  ('compat/posix/src/winsock.cc',b'(flags & kMsgCmsgCloexec) != 0);',b'false);'),
  ('compat/posix/src/unix_rights.cc',b'AdoptFileDescription(h, shared, static_cast<DescriptorKind>(kind), close_on_exec)',b'AdoptFileDescription(h, shared, static_cast<DescriptorKind>(kind), false)'),
  ('probes/binder/binder_probe.cc',b'retained_files.size() == 9',b'retained_files.size() == 6'),
  ('compat/posix/src/system_properties.cc',b'__atomic_load_n(&header->generation, __ATOMIC_ACQUIRE)',b'__atomic_load_n(&header->generation, __ATOMIC_RELAXED)'),
  ('compat/posix/src/system_properties.cc',b'Revision{area, area->cached_generation}',b'Revision{area, 0}'),
  ('compat/posix/src/system_properties.cc',b'EnvironmentEquals(L"GESHER_BINDER", session.substr(7))',b'true'),
  ('compat/posix/src/system_properties.cc',b'SetWideEnvironment(L"GESHER_PROPERTY_ROOT", root.c_str())',b'_wputenv_s(L"GESHER_PROPERTY_ROOT", root.c_str())'),
  ('compat/posix/include/gesher/system_properties.h',b'uint32_t generation = 0;',b'uint64_t generation = 0;'),
  ('tools/net-tcp-priority/qualified/native13-property-source.patch',b'@@',b'@x'),
  # #421's build.prop filter admits its exact reviewed delta only.
  ('compat/posix/src/system_properties.cc',b'if (!ServedBuildProperty(key, value)) continue;',b'if (!ServedBuildProperty(key, value)) {}'),
  (GLES_PROPERTY_PATCH,b'@@',b'@x'),
  (NETLINK_PATCH,b'@@',b'@x'),
  ('compat/posix/src/socket.cc',b'if (protocol != NETLINK_ROUTE)',b'if (protocol == NETLINK_ROUTE)'),
  # The memmem normalization admits its exact reviewed delta only. The old
  # whole-file digest must still reject any unrelated shim change afterwards.
  ('compat/bionic/shim.cc',b'size_t n = std::strlen(s) + 1;',b'size_t n = std::strlen(s) + 2;'),
  ('compat/bionic/shim.cc',b'return const_cast<void*>(search::FindBytes(h, hn, n, nn));',b'return const_cast<void*>(search::FindBytes(n, nn, h, hn));'),
  ('compat/bionic/shim.cc',b'#include "memmem_search.h"\n',b''),
  ('compat/bionic/shim.cc',b'#include "memmem_search.h"\n',b'#include "memmem_search.h"\n#include "memmem_search.h"\n'),
  (MEMMEM_HEADER,b'if (needle_size == 0) return haystack_data;',b'if (needle_size == 0) return nullptr;'),
  # The memfd lease transition admits its exact reviewed delta only: the
  # signature, the seal's exclusive lease probe, the lease's hand-off to split
  # views, and the pinned reverse transition itself.
  ('compat/posix/src/internal.h',b'std::shared_ptr<void>* write_lease);',b'std::shared_ptr<void>* lease);'),
  ('compat/posix/src/memfd_seals.cc',b'LockByte(file, kLeaseByte, true, false)',b'LockByte(file, kLeaseByte, false, false)'),
  ('compat/posix/src/memfd_seals.cc',b'constexpr int kLockAttempts = 5000;',b'constexpr int kLockAttempts = 50;'),
  ('compat/posix/src/mman.cc',b'r.write_lease = view.write_lease;',b'r.write_lease.reset();'),
  ('compat/posix/src/mman.cc',b'if (it->second.kind == Kind::kView) it->second.write_lease = write_lease;',b'if (it->second.kind == Kind::kView) {}'),
  (MEMFD_LEASE_PATCH,b'@@',b'@x'),
  (MEMFD_LEASE_PATCH,b'+constexpr int kLockAttempts = 5000;',b'+constexpr int kLockAttempts = 50;'),
  *STDIO_LINE_NEGATIVE_CASES,
  *STDIO_SCAN_NEGATIVE_CASES,
  *SNPRINTF_BOUNDS_NEGATIVE_CASES,
 ]
 def memfd_lease_repin(name,data):
  # Refresh only the receipt's digest of the mutated file (and the receipt's
  # own pin); the reverse transition stays the reviewed one.
  qualification=json.loads(original(ROOT/MEMFD_LEASE_QUALIFICATION))
  if name==MEMFD_LEASE_PATCH:qualification['patch_sha256']=hashlib.sha256(data).hexdigest()
  else:qualification['source_sha256'][name]=hashlib.sha256(data).hexdigest()
  return (json.dumps(qualification,indent=2)+'\n').encode()
 repinned_memfd_lease=0
 binder_cases=[
  ('compat/binder/binderd.cc',b'  Out& out;',b'  Out out;'),
  ('compat/binder/binderd.cc',b'command_start(output.bytes.size())',b'command_start(0)'),
  ('compat/binder/binderd.cc',b'memcpy(reply.bytes.data() + sizeof(uint32_t), &ret, sizeof(int32_t));',
   b'memcpy(reply.bytes.data(), &ret, sizeof(int32_t));'),
  ('compat/binder/binderd.cc',b'if (n == 0) return true;',b'if (n == 0) return false;'),
  ('compat/binder/binderd.cc',b'if (accepted && !thread->pending_buffer_claimed) return -EINVAL;',
   b'if (false && !thread->pending_buffer_claimed) return -EINVAL;'),
 ]
 cases+=binder_cases+[
  ('tools/net-tcp-priority/qualified/binder-output-source.json',b'"native_runtime_approval": false',b'"native_runtime_approval": true'),
  ('tools/net-tcp-priority/qualified/binder-output-source.patch',b'@@',b'@x'),
 ]
 for name,before,after in cases:
  target=ROOT/name
  assert before in original(target),name
  def changed(path):
   raw=original(path)
   return raw.replace(before,after,1) if path==target else raw
  # A refreshed new-file hash alone cannot authorize property or memfd lease
  # behavior changes: the pinned reverse transition and original OFD receipt
  # must reject them too.
  lease=name in MEMFD_LEASE_PATHS or name==MEMFD_LEASE_PATCH
  for repin in (False,True) if name=='compat/posix/src/system_properties.cc' or lease else (False,):
   pins={'GLES_PROPERTY_SHA256':GLES_PROPERTY_SHA256,'MEMFD_LEASE_QUALIFICATION_SHA256':MEMFD_LEASE_QUALIFICATION_SHA256}
   reads=changed
   # The current file's pin is GLES_PROPERTY_SHA256: refreshing it must leave the
   # reverse transitions (#421's, then NATIVE-13's) to reject the change.
   if repin and not lease:pins['GLES_PROPERTY_SHA256']=hashlib.sha256(changed(target)).hexdigest()
   if repin and lease:
    lease_sources=usb_preceding
    data=changed(target)
    if name in usb_preceding:
     # The ordinary mutation above tests the full combined gate. For this
     # repinned control, test the memfd reversal on its actual input after USB
     # normalization; the independent USB source pin must not mask a broken
     # memfd reverse-transition check in the overlapping header.
     assert before in usb_preceding[name],name
     data=usb_preceding[name].replace(before,after,1)
     lease_sources={**usb_preceding,name:data}
    receipt=memfd_lease_repin(name,data)
    pins['MEMFD_LEASE_QUALIFICATION_SHA256']=hashlib.sha256(receipt).hexdigest()
    reads=lambda path,receipt=receipt,changed=changed:receipt if path==ROOT/MEMFD_LEASE_QUALIFICATION else changed(path)
    repinned_memfd_lease+=1
   rejected=None
   with patch.object(Path,'read_bytes',reads), patch.dict(globals(),pins):
    try:
     if repin and lease and name in usb_preceding:preceding_memfd_lease_sources(lease_sources)
     else:check()
    except (AssertionError,subprocess.CalledProcessError) as error:rejected=error
   assert rejected is not None,'unqualified mutation accepted: '+name+' repin='+str(repin)
   # A repinned receipt passes its own digest checks, so the rejection must
   # come from the reverse transition, not from the stale pin.
   if repin and lease:
    message=str(rejected)
    assert 'qualification changed' not in message and 'transition patch changed' not in message and message!=name,(name,message)
 # Exercise each original leaf at its exact normalized input, in addition
 # to the complete-chain mutations above. Repinning an outer-stage digest must
 # not hide the failure in a different stage before the intended guard executes.
 line_current=current_stdio_line_inputs()
 repinned_composed=0
 for name,before,after in STDIO_LINE_NEGATIVE_CASES:
  if name not in COMPOSED_STDIO_PATHS:continue
  changed_source=line_current[name].replace(before,after,1)
  assert changed_source!=line_current[name],name
  pins=dict(COMPOSED_STDIO_SHA256);pins[name]=hashlib.sha256(changed_source).hexdigest()
  rejected=None
  with patch.dict(globals(),COMPOSED_STDIO_SHA256=pins):
   try:preceding_composed_stdio_sources({**line_current,name:changed_source})
   except (AssertionError,subprocess.CalledProcessError) as error:rejected=error
  assert rejected is not None and str(rejected)!=name,'repinned composed line mutation accepted or rejected by stale pin: '+name
  repinned_composed+=1
 print('COMPOSED_STDIO_REPINNED_CONTROLS_PASS cases='+str(repinned_composed))
 scan_path=COMPOSED_SCAN_PATHS[0]
 scan_current=original(ROOT/scan_path)
 historical_source=subprocess.check_output(['git','-C',str(ROOT),'show',COMPOSED_SCAN_PRIOR_FEATURE+':'+scan_path])
 manifest=ROOT/'tools/net-tcp-priority/qualified/stdio-scan-source.json'
 for name,before,after in STDIO_SCAN_NEGATIVE_CASES[:4]:
  assert name==scan_path and scan_current.count(before)==historical_source.count(before)==1
  changed=scan_current.replace(before,after,1)
  pins={scan_path:hashlib.sha256(changed).hexdigest()}
  rejected=None
  with patch.dict(globals(),COMPOSED_SCAN_SHA256=pins):
   try:preceding_composed_scan_sources({scan_path:changed})
   except (AssertionError,subprocess.CalledProcessError) as error:rejected=error
  assert rejected is not None and str(rejected)!=name,'repinned composed scanner mutation accepted or rejected by stale pin: '+before.decode()
  changed_historical=historical_source.replace(before,after,1)
  qualification=json.loads(original(manifest));qualification['source_sha256'][name]=hashlib.sha256(changed_historical).hexdigest()
  raw=(json.dumps(qualification,indent=2)+'\n').encode()
  rejected=None
  with patch.object(Path,'read_bytes',lambda path:raw if path==manifest else original(path)),patch.dict(globals(),STDIO_SCAN_QUALIFICATION_SHA256=hashlib.sha256(raw).hexdigest()):
   try:preceding_stdio_scan_source(changed_historical)
   except (AssertionError,subprocess.CalledProcessError) as error:rejected=error
  assert rejected is not None and str(rejected)!=name,'repinned historical scanner mutation accepted or rejected by stale pin: '+before.decode()
 qualification=json.loads(original(manifest));qualification['native_runtime_approval']=True
 raw=(json.dumps(qualification,indent=2)+'\n').encode();rejected=False
 with patch.object(Path,'read_bytes',lambda path:raw if path==manifest else original(path)),patch.dict(globals(),STDIO_SCAN_QUALIFICATION_SHA256=hashlib.sha256(raw).hexdigest()):
  try:preceding_stdio_scan_source(historical_source)
  except (AssertionError,subprocess.CalledProcessError):rejected=True
 assert rejected,'repinned scanner runtime approval accepted'
 print('COMPOSED_SCAN_REPINNED_CONTROLS_PASS current=4 historical=4 runtime_approval=1')
 # Merely refreshing the new source hash must not admit changed parser,
 # destination-publication, capacity or unrelated unbounded-formatting bytes.
 # The exact reverse patch and published baseline independently reject them.
 target=ROOT/SNPRINTF_BOUNDS_SOURCE
 for name,before,after in SNPRINTF_BOUNDS_NEGATIVE_CASES[:-1]:
  assert ROOT/name==target and original(target).count(before)==1
  changed_source=original(target).replace(before,after,1)
  def repinned(path):return changed_source if path==target else original(path)
  rejected=None
  with patch.object(Path,'read_bytes',repinned), patch.dict(globals(),SNPRINTF_BOUNDS_SHA256=hashlib.sha256(changed_source).hexdigest()):
   try:preceding_snprintf_bounds_source()
   except (AssertionError,subprocess.CalledProcessError) as error:rejected=error
  assert rejected is not None and str(rejected)!=name,'repinned snprintf mutation accepted or rejected by stale pin: '+before.decode()
 print('SNPRINTF_BOUNDS_REPINNED_CONTROLS_PASS cases=5')
 # Fresh source/manifest digests alone cannot authorize a changed broker:
 # the reversible patch and exact original OFD bytes must still agree.
 target=ROOT/'compat/binder/binderd.cc'
 manifest=ROOT/'tools/net-tcp-priority/qualified/binder-output-source.json'
 for name,before,after in binder_cases:
  assert ROOT/name==target and original(target).count(before)==1
  changed_source=original(target).replace(before,after,1)
  qualification=json.loads(original(manifest))
  qualification['source_sha256'][name]=hashlib.sha256(changed_source).hexdigest()
  changed_manifest=(json.dumps(qualification,indent=2)+'\n').encode()
  def repinned(path):
   return changed_source if path==target else changed_manifest if path==manifest else original(path)
  rejected=False
  with patch.object(Path,'read_bytes',repinned), patch.dict(globals(),BINDER_OUTPUT_QUALIFICATION_SHA256=hashlib.sha256(changed_manifest).hexdigest()):
   try:check()
   except (AssertionError,subprocess.CalledProcessError):rejected=True
  assert rejected,'repinned Binder output mutation accepted: '+before.decode()
 qualification=json.loads(original(manifest))
 qualification['native_runtime_approval']=True
 changed_manifest=(json.dumps(qualification,indent=2)+'\n').encode()
 def elevated(path):return changed_manifest if path==manifest else original(path)
 rejected=False
 with patch.object(Path,'read_bytes',elevated), patch.dict(globals(),BINDER_OUTPUT_QUALIFICATION_SHA256=hashlib.sha256(changed_manifest).hexdigest()):
  try:check()
  except (AssertionError,subprocess.CalledProcessError):rejected=True
 assert rejected,'repinned Binder output runtime approval accepted'
 # A changed modern source hash cannot admit a modified output operation or
 # unrelated main admission code: the same original patch must still restore
 # the exact published main endpoint after the historical receipt is checked.
 composed_cases=binder_cases+[
  ('compat/binder/binderd.cc',b'if (g_sandbox && ClientIsRestricted(pipe)) {',
   b'if (false && ClientIsRestricted(pipe)) {'),
  ('compat/binder/binderd.cc',b'if (uid < 10000 || !known || actual != pid) {',
   b'if (uid < 10000 || !known) {'),
 ]
 for name,before,after in composed_cases:
  # This stage checks main's endpoint after the independently checked USB
  # transition is reversed. Exercise its control at that same boundary.
  composed=usb_preceding[name]
  assert ROOT/name==target and composed.count(before)==1
  altered=composed.replace(before,after,1)
  rejected=False
  with patch.dict(globals(),COMPOSED_BINDER_OUTPUT_SHA256=hashlib.sha256(altered).hexdigest()):
   try:preceding_composed_binder_output_source(altered)
   except (AssertionError,subprocess.CalledProcessError):rejected=True
  assert rejected,'repinned composed Binder mutation accepted: '+before.decode()
 print('TCP_SELECTED_TRANSITION_NEGATIVE_CONTROLS_PASS cases='+str(len(cases))+' repinned_property_controls=5 repinned_memfd_lease_controls='+str(repinned_memfd_lease)+' repinned_binder_output_controls=6 repinned_composed_binder_controls='+str(len(composed_cases)))
if __name__=='__main__':
 import sys
 check()
 if '--selftest' in sys.argv:
  usb_main513_negative_controls()
  storage_policy_negative_controls()
  transition_negative_controls()
  import runpy
  runpy.run_path(str(ROOT/'tools/net-tcp-priority/test-shared-stdio-policy.py'),run_name='__main__')
  runpy.run_path(str(ROOT/'tools/net-tcp-priority/test-memrchr-policy.py'),run_name='__main__')
  runpy.run_path(str(ROOT/'tools/net-tcp-priority/test-tsf-metadata-policy.py'),run_name='__main__')
  runpy.run_path(str(ROOT/'tools/net-tcp-priority/test-scandir-policy.py'),run_name='__main__')
