"""Pure selected-consumer source/DAG preservation checks; no native execution."""
from pathlib import Path
import ast,hashlib,json,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
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
OFD_QUALIFICATION_SHA256 = 'b009b50f04a8e076bd89527a7237da02f2f547d8e41ab88e2841aea2b9e61d15'
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

def preceding_shared_stdio_sources(qualification):
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
 current={};baseline={}
 for path in paths:
  current[path]=(ROOT/path).read_bytes()
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
def preceding_logging_property_source(source):
 path='compat/posix/src/system_properties.cc'
 source=preceding_gles_property_source(source)
 assert hashlib.sha256(source).hexdigest()==PROPERTY_REVISION_SHA256,path
 header='compat/posix/include/gesher/system_properties.h'
 assert hashlib.sha256((ROOT/header).read_bytes()).hexdigest()==PROPERTY_REVISION_HEADER_SHA256,header
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
MEMFD_LEASE_QUALIFICATION_SHA256 = 'f80baede63b053f6b4a7d845a94f83b31a6e45c8129746ca1a5b85cd902f9443'
MEMFD_LEASE_BASELINE='e074b67b2f83ce9128c11713171329e9fbbf6739'
MEMFD_LEASE_PATHS=['compat/posix/src/internal.h','compat/posix/src/memfd_seals.cc','compat/posix/src/mman.cc']
def preceding_memfd_lease_sources(usb_block):
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
 current={path:usb_block[path] if path in usb_block else (ROOT/path).read_bytes()
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
USB_BLOCK_TRANSITION_SHA256 = '1199c0f0e2c6a41125362281a827c017ac01f674987af70b3b4db5ab6827a98f'
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
 'compat/posix/src/system.cc',
 'compat/posix/src/system_properties.cc',
 'compat/posix/src/unistd.cc',
 'compat/posix/src/unix_rights.cc',
 'tests/art-temp/entry_test.cc',
 'tests/native/ofd_transfer_host_test.cc.in',
 'tools/test_environment_lookup_contracts.py',
 'tools/test_temp_directory.py',
)


def preceding_usb_block_sources(folder):
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
  assert hashlib.sha256((ROOT/path).read_bytes()).hexdigest() == digest, path

 preceding = {}
 with tempfile.TemporaryDirectory(prefix='gesher-usb-block-qualification-') as temporary:
  work = Path(temporary)
  for path in paths:
   current = (ROOT/path).read_bytes()
   assert hashlib.sha256(current).hexdigest() == transition['after_sha256'][path], path
   (work/path).parent.mkdir(parents=True, exist_ok=True)
   (work/path).write_bytes(current)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','--check','-'],
                 input=delta,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','-'],
                 input=delta,cwd=work,check=True,capture_output=True)
  for path in paths:
   preceding[path] = (work/path).read_bytes()
   historical = subprocess.check_output(['git','-C',str(ROOT),'show',
                                        USB_BLOCK_PATH_BASES.get(path, USB_BLOCK_BASE)+':'+path])
   assert preceding[path] == historical, path
 return preceding


def preceding_ofd_sources():
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
 usb_block=preceding_usb_block_sources(folder)
 memfd_lease=preceding_memfd_lease_sources(usb_block)
 shared_stdio=preceding_shared_stdio_sources(qualification)
 assert memfd_lease.keys().isdisjoint(shared_stdio),'source qualifications overlap'
 for path,digest in qualification['source_sha256'].items():
  if path in shared_stdio: actual=shared_stdio[path]
  elif path in memfd_lease: actual=memfd_lease[path]
  elif path in usb_block: actual=usb_block[path]
  else: actual=(ROOT/path).read_bytes()
  if path=='compat/posix/src/system_properties.cc':actual=preceding_logging_property_source(actual)
  if path=='compat/bionic/shim.cc':actual=preceding_memmem_source(actual)
  assert hashlib.sha256(actual).hexdigest()==digest,path
 with tempfile.TemporaryDirectory(prefix='gesher-ofd-qualification-') as temporary:
  work=Path(temporary)
  for path in paths:
   (work/path).parent.mkdir(parents=True,exist_ok=True)
   (work/path).write_bytes(usb_block[path] if path in usb_block else (ROOT/path).read_bytes())
  subprocess.run(['git','apply','--reverse','--unidiff-zero','--check','-'],input=patch,cwd=work,check=True,capture_output=True)
  subprocess.run(['git','apply','--reverse','--unidiff-zero','-'],input=patch,cwd=work,check=True,capture_output=True)
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
  current=(ROOT/path).read_bytes()
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
REVIEWED_PATHS=('compat/posix/src/fd.cc','compat/posix/src/socket.cc','compat/posix/src/socket_internal.h','compat/posix/src/system.cc','compat/posix/src/winsock.cc')
def old(path):return subprocess.check_output(['git','-C',str(ROOT),'show',BASE+':'+path])
def check():
 preceding=preceding_ofd_sources()
 assert hashlib.sha256((ROOT/OWNER_THREAD_DIAGNOSTIC_PATH).read_bytes()).hexdigest()==OWNER_THREAD_DIAGNOSTIC_SHA256,OWNER_THREAD_DIAGNOSTIC_PATH
 assert hashlib.sha256((ROOT/FD_POLL_SCRATCH_HEADER).read_bytes()).hexdigest()==FD_POLL_SCRATCH_SHA256,FD_POLL_SCRATCH_HEADER
 paths=subprocess.check_output(['git','-C',str(ROOT),'diff','1745e99d7f2849ba81784545e07d60cb2b7800c3^','1745e99d7f2849ba81784545e07d60cb2b7800c3','--name-only'],text=True).splitlines()
 paths+=['.github/workflows/net-tcp-coherent-component.yml','tests/net-tcp-priority/contract.cc','tests/net-tcp-priority/native_udp_control.cc','tests/native2/net_udp_trunc.cpp','tests/native2/net_udp_fault_observer.c','tools/net-tcp-priority/qualified/TsfJobSupervisor.cs','tools/windows4/TsfJobSupervisor.cs','tools/windows4/TsfJobWorker.cs','tools/windows4/test-tsf-job-supervisor.ps1','tools/ci/receipt-diagnostics.ps1','tools/net-tcp-priority/import-policy.json','tools/net-tcp-priority/import_policy.py','tools/net-tcp-priority/build-guests.py','tools/net-tcp-priority/prepare-ndk.py']
 for path in paths:
  epoch=(FD_BASE if path=='compat/posix/src/fd.cc' else
         SOCKET_BASE if path=='compat/posix/src/socket.cc' else
         WINSOCK_BASE if path=='compat/posix/src/winsock.cc' else
         CONTROL_BASE if path=='tools/windows4/test-tsf-job-supervisor.ps1' else BASE)
  expected=subprocess.check_output(['git','-C',str(ROOT),'show',epoch+':'+path])
  actual=preceding.get(path,(ROOT/path).read_bytes())
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
 print('TCP_SELECTED_POLICY_PASS exact_OFD_stdio_pipe_transition exact_property_revision_transition exact_gles_property_transition exact_memfd_lease_transition reviewed_pipe_control_errors reviewed_process_lifetime_socket_registry reviewed_outer_poll_scratch_exact reviewed_priority_wake_confirmation_only original_socket_ownership_and_native_poll=EXACT original_product3 reviewed_nested_epoll_fd reviewed_device_lookup_and_stream_retirement_only reviewed_sendmsg_control reviewed_statvfs_affinity_file_advice_allocation_writev_and_memfd_only reviewed_fcntl_argument_selection_only canonical_Recv originalworkflow other_fixture_assertions_deadlines=EXACT reviewed_select_and_calibrated_owner_census controller_execution_tail original_import_NDK_build_roles=EXACT new_two_supplier_DAG=BOUNDED source12_patch2_exact native_processes_started=0')
def transition_negative_controls():
 from unittest.mock import patch
 original=Path.read_bytes
 usb_preceding=preceding_usb_block_sources(ROOT/'tools/net-tcp-priority/qualified')
 cases=[
  ('compat/posix/src/fd.cc',b'--state->description->descriptor_refs == 0',b'--state->description->descriptor_refs == 1'),
  ('compat/posix/src/fd.cc',b'return TransferPipe(snapshot, bytes, count, false);',b'return 0;'),
  ('compat/posix/src/system.cc',b'gesher::ws::RecvOwned(state.socket,',b'gesher::ws::SendOwned(state.socket,'),
  ('compat/posix/src/pipe_io.cc',b'if (!count) return 0;',b'if (!count) return 1;'),
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
 ]
 def memfd_lease_repin(name,data):
  # Refresh only the receipt's digest of the mutated file (and the receipt's
  # own pin); the reverse transition stays the reviewed one.
  qualification=json.loads(original(ROOT/MEMFD_LEASE_QUALIFICATION))
  if name==MEMFD_LEASE_PATCH:qualification['patch_sha256']=hashlib.sha256(data).hexdigest()
  else:qualification['source_sha256'][name]=hashlib.sha256(data).hexdigest()
  return (json.dumps(qualification,indent=2)+'\n').encode()
 repinned_memfd_lease=0
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
 print('TCP_SELECTED_TRANSITION_NEGATIVE_CONTROLS_PASS cases='+str(len(cases))+' repinned_property_controls=5 repinned_memfd_lease_controls='+str(repinned_memfd_lease))
if __name__=='__main__':
 import sys
 check()
 if '--selftest' in sys.argv:
  transition_negative_controls()
  import runpy
  runpy.run_path(str(ROOT/'tools/net-tcp-priority/test-shared-stdio-policy.py'),run_name='__main__')
