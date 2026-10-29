# TCP urgent priority readiness (ADR0070, test93)

Exclusive source: `wip/net-tcp-priority`, based on qualified coherent source
`c254c6c19fb3c8553208a096116eab7350d4de49`. No integration acceptance yet.

## Contract

Linux `poll(POLLPRI)`, `select(exceptfds)` and level `epoll(EPOLLPRI)` report
queued TCP urgent data with SO_OOBINLINE both disabled and enabled. A normal
prefix can remain readable while urgent readiness is present; consuming the
out-of-band byte clears priority immediately, before consuming that prefix.
Inline consumption preserves the urgent byte in stream order. Readiness must
not consume payload or SO_ERROR or alter socket blocking/notification state.

Independent Linux source `tests/net-tcp-priority/contract.cc` compiles and runs
only in CI. It is also intended for the actual Gesher POSIX host target and
both NDK executable ABIs, rather than a translated model of the implementation.
It covers no-data and normal controls, bounded finite waits, queue/re-poll,
simultaneous normal/urgent data, descriptor aliases, multiple descriptors,
level epoll, EOF, and delayed urgent arrival from another thread.

## Backend choice prerequisite

Microsoft documents that WSAPoll rejects POLLPRI. Winsock select exception
readiness includes failed nonblocking connect as well as OOB; it suppresses
OOB exceptions entirely for SO_OOBINLINE. Its SIOCATMARK semantics differ
from Linux and must be characterized rather than assumed. The independent
WinSDK oracle records normal/urgent queue transitions with actual TCP_INFO
receive counters, prefix and urgent consumption, and refused-connect state.
No Gesher runtime is loaded by that diagnostic. Runtime source changes and
fresh matched host/DLL/both-ABI CI are still required after it.

Primary references:
- https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsapoll
- https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select
- https://learn.microsoft.com/en-us/windows/win32/winsock/protocol-independent-out-of-band-data-2
- https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-ioctls
- https://kernel.googlesource.com/pub/scm/linux/kernel/git/brauner/man-pages/+/refs/heads/master/man7/tcp.7

## Actual diagnostic evidence

All compilation/execution below was CI-only. Integrator reviewed source before
push; each push checked the palmoni5 account and used gh-authenticated HTTPS.
No artifacts were downloaded locally and no production runtime was reused.

- `0efbf7eca4c870cdd10b2216ed744e780e75cb76` / `40849173838`:
  independent Linux contract PASS, both Windows urgent queue/consumption
  observations recorded. Overall RED because refused-connect notification did
  not arrive within the original two-second diagnostic bound. No unchanged retry.
- `388b1868cc3f329c2e62b53b5b128eca2a7494c0` / `40849990611`:
  GREEN Linux plus both Windows. Added suffix/boundary observations and held
  non-listening port to exclude reuse. Refusal notification arrived near two
  seconds inside the changed five-second bound; getpeername failed with10057.
- `3b92a7f0555aef523774a902b28cebf15151cf30` / `40850431776`:
  GREEN Linux plus both Windows. Additional non-consuming WSAPoll POLLRDBAND
  observation at every existing phase. Non-inline urgent yields512 until
  MSG_OOB consumes the byte, including after the normal prefix is consumed.
  Native inline mode yields0 in every phase despite proved urgent arrival.

Native Windows SIOCATMARK is0 while out-of-band data remains unread and1
otherwise. In inline mode it is1 in every phase. FIONREAD includes normal
bytes on both sides of the urgent byte: prefix6+urgent1+suffix4 reports10,
and reports4 after reading the prefix while urgent data remains unread.
Therefore neither ioctl can recover the urgent byte's position for a safe
logical-inline implementation. Blanket normal-read or exception readiness
must not be converted to Linux priority readiness. SO_ERROR remains untouched
by all diagnostic observations.

Actual raw logs are preserved in C:/work/gesher-ci-review:
`tcp-priority-40849173838-{linux,windows2022,windows2025}.log`,
`tcp-408499-win2022.log`, `tcp-408499-win2025.log`,
`tcp-408504-win2022.log`, `tcp-408504-win2025.log`.

## Pending production source and remaining work

Generic `fd.cc` dropped POLLPRI in both request and result translation.
The pending change forwards those bits. The Winsock source requests its
supported POLLRDBAND for neutral priority interest and maps that result back
to priority, independently of normal POLLRDNORM, errors or connect failure.
This removes the old incorrect treatment of band data as normal readiness.
It preserves WSAPoll's existing ownership, real timeout and cancellation
behavior without installing socket events, spinning, consuming OOB or reading
SO_ERROR. select/bionic select and epoll already preserve exceptional masks.

The same contract source has a CMake POSIX host target. New refused-connect,
SO_ERROR consumption and closed-descriptor controls are source-only until
fresh Linux and production CI validate them. Both NDK ABIs must be built and
run against a fresh matching host and translator DLL with strict source/run/
attempt/compiler/member-role digest evidence. No production implementation
has passed CI yet. The full contract fixture deliberately continues to require
SO_OOBINLINE1; it must not be silently skipped to claim complete parity.

The existing SocketFd table copies values and ws::Duplicate creates another
provider handle to the same endpoint. Any future logical SO_OOBINLINE state
must live in shared open-description storage copied by dup/dup2, while accept
creates a new description with inherited option values. A receive-side design
must preserve the prefix/urgent/suffix boundary, MSG_PEEK, MSG_OOB rejection
when inline, scatter/gather, concurrent readers, timeout and close lifetime.
The public Winsock observations above do not yet supply that boundary; an
approximate userspace byte queue would corrupt ordering and is not implemented.

Strict coherent component and canonical whole-suite caller/guard plumbing
remain pending. No all-Net4, all-poll, ancillary, hardware, main or release
closure is implied. Inventory and historical proofs are unchanged.

## Subsequent source and actual observations

`7b825fcab705b41b9405aa86a9f40af3fb782e8a` introduced the real non-inline
POLLPRI mapping and the one-producer component. Component `40852406540` is
RED: Linux full reference passed; the actual host/probe, translator DLL and
both NDK ELFs compiled, but the producer's import-role assertion rejected
WINHTTP.dll and api-ms-win-core-synch-l1-2-0.dll before artifact upload.
Both Windows runtime jobs were skipped. This supplies no production runtime
verdict. Raw failed-job log: `tcp-priority-40852406540-failed.log`.

Changed source `2152fb7af20f871a36c515b641837ac1baccb11b` replaces the shared
import allowlist with role-specific policy and unknown/private/path/empty/
wrong-role controls. WINHTTP belongs only to gesher-exec's existing
MediaPlayer backend (`cmake/aosp/media.cmake`, player/http_source.cc), not to
the POSIX probe or translator DLL. Synchronization's API-set dependency is
individually justified by the existing POSIX and translator CMake links.
The producer prints all three actual import sets before enforcing policy;
probe and DLL role acceptance remains unproved until actual CI.

Independent boundary diagnostic `40852850367` / `a873ca8b` is RED on both
Windows because the hypothesis that a large normal recv stops before urgent
was disproved. Actual MSG_PEEK with32-byte capacity returned the six-byte
prefix; subsequent normal recv with32-byte capacity returned ten bytes
`normaltail`, crossing the urgent byte's stream position. This failed source
was not retried. Raw log: `tcp-priority-40852850367-all.log`.

Changed diagnostic `40853977336` / `633b04cb` is GREEN (Linux and both
Windows). It preserves that native counterexample as an explicit observation
and tests a separate receive capped to the peeked prefix length. Both
Windows returned six prefix bytes for the capped receive, then a normal
MSG_PEEK at the mark returned the four-byte suffix while SIOCATMARK remained0,
POLLRDBAND remained512 and OOB|PEEK returned the unchanged urgent byte33.
The uncapped crossing receive likewise left the urgent byte independently
available. Raw log: `tcp-priority-40853977336-all.log`.

Consequently, the characterized native APIs do not yet expose a safe Linux
position-at-urgent predicate for logical-inline delivery. In particular,
neither SIOCATMARK==0 nor a positive normal MSG_PEEK distinguishes unread
normal prefix from suffix after the urgent position. A receive capped to an
observed peek length prevents that particular crossing but does not establish
which side of the urgent byte the data occupies. No logical-inline code is
implemented from these observations. Repeated urgent indications, option
changes and exact mark/peek/WAITALL/dup/close races require further independent
packet-ordering evidence before any broader implementation or claim.

## Refused-connect interest diagnosis

Actual component40854839124/source2152 is RED on both Windows: the host's
real non-inline urgent scenario passed, then the strict refused-connect
control failed; neither NDK ABI ran. Diagnostics9c1b8852 were independently
reviewed before push. Component40857235358 retains that failure and proves
the host PRI-only poll returns0/revents0 after5004/5015ms, despite a completed
refusal (getpeername returns ENOTCONN107). The printed errno115 is stale
EINPROGRESS from connect, not a poll error. No SO_ERROR was consumed.

Independent WinSDK40857235335 on the same diagnostic source is GREEN on
both Windows. RDBAND-only returns0/revents0 after5015/5000ms; subsequent
select reports exception readiness and getpeername returns10057. Even after
completed refusal, band-only remains0, while band+normal+write interest
returns1/revents19 (native ERR|HUP|WRNORM). Raw logs are
`tcp-408572-{oracle,host}{2022,2025}.log` in the preserved CI-review folder.
This distinguishes native interest gating from the old two-second race and
from an erroneous priority bit. Increasing the bound or accepting timeout
would preserve the defect.

The pending correction requests internal write interest only on unconnected,
non-listening stream sockets, using non-consuming SO_TYPE/SO_ACCEPTCONN and
getpeername observations. Unrequested write results are filtered. The existing
PollFds loop retains its deadline and re-evaluates sockets after a successful
connect's internal write wake. No socket modes, event subscriptions, OOB or
SO_ERROR are changed. Fresh stream HUP, idle listener, unconnected UDP,
successful nonblocking connect and connected finite waits are strict added
controls. Actual fresh CI on the corrected host and both NDK ABIs remains
required; no source-only implementation is accepted by this entry.

Correction3301dd66/component40859184041 is RED: both actual hosts still pass
the non-inline urgent scenario, then the added fresh-stream HUP control fails
before refused/alias controls or either ABI can execute. The independent
expanded native40859184013 is GREEN and records fresh TCP band+write rc0;
completed refusal returns ERR|HUP|WRNORM only when write interest is included
(band-only and band+read both remain0). Raw logs are
`tcp-408591-{oracle,component}{2022,2025}.log`. No unchanged retry occurred.

The changed pending source retains the strict initial HUP assertion. A shared
atomic Initial/Connecting/Connected/Failed record is allocated at socket registration, including
the internal DNS registration path, and follows dup/dup2 through SocketFd's
shared ownership. Connect marks Connecting before entering Winsock; success
records Connected, EINPROGRESS/EALREADY/EINTR preserve Connecting, and other
real-call failures record Failed. Actual native peer success records Connected;
native ERR/HUP changes only an active Connecting record to Failed. The backend
distinguishes an initial unconnected stream from a pending connection using
that record plus actual SO_TYPE, SO_ACCEPTCONN and getpeername. Initial HUP is
reported without waiting; pending connect errors retain conditional write
interest. Both PollFds and MSG_DONTWAIT pass the same lifetime record. Existing
finite timeout/cancellation behavior and error ownership remain unchanged.
Failed endpoints retain HUP after caller error consumption and continue to
request native write status so unread ERR remains visible before consumption.
Fresh alias/bound-stream controls and a connect begun through an existing
alias are added; socket.cc's actual compile command joins the strict producer
and consumer roles. This is not shared O_NONBLOCK or inline-OOB parity.
Strict post-SO_ERROR HUP and successful-connect/alias/EOF controls join the
Linux reference before actual Windows acceptance. Failed TCP reconnect state
is unspecified by connect(2); TCP AF_UNSPEC disconnect/reconnect and concurrent
multiple connect attempts are not qualified by this bounded fixture.

Linux-first characterization40862384927 at74180cde is GREEN on all three
jobs. The actual Linux full fixture passes both inline modes, initial HUP16,
four finite waits (60-61ms), refused ERR|HUP24 without PRI, alias/error
ownership, and consumed-error HUP16. The final all-contract marker is present;
raw Linux log is `tcp-408623-linux.log`. This validates the added reference
expectations, not the pending Windows production lifecycle implementation.

Actual lifecycle component40862743282/source538e22c4 is RED on both Windows.
The host passes initial HUP16, listener/UDP/successful-connect/connected finite
waits (61-63ms), the blocking MSG_DONTWAIT control, successful EOF/alias and
non-inline urgent controls. Refused priority poll now returns1/ERR|HUP24 near
two seconds. The strict subsequent SO_ERROR==ECONNREFUSED assertion fails;
neither guest ABI executes. Raw logs: `tcp-408627-win{2022,2025}.log`.

Changed diagnostics8d9526a4 preserve that exact assertion and print actual
getter rc/value/errno/length. Native40864241596 and actual component40864241587
are pending. Six independent refused endpoints isolate select-only, write
poll-only, poll/getpeername, a pre-connect alias, a post-refusal alias and the
full metadata/alias sequence. Only their explicit final caller reads SO_ERROR
twice. Diagnostic labels now say `SO_ERROR=not-read`: absence of a direct
getter call is not proof that another native operation preserved the error.
No production workaround or weakened ownership expectation is accepted.

Adjacent source-only gap: host SelectMs and bionic's SelectImpl each filter
ERR/HUP from exceptfds-only results after one poll; an error-only wake may
therefore return0 before the caller's select deadline. That broader finite
select behavior is not qualified by the connected urgent fixture and remains
open alongside inline readiness and the other documented network gaps.

Ownership diagnostic40864241596 is GREEN on both Windows. Select-only,
write-poll-only, poll/getpeername and post-refusal-alias cases each return
native SO_ERROR10061 on both explicit reads. The pre-connect-alias/close case
and full metadata/alias case return0 on both reads of the original handle.
Actual component40864241587 is RED: both hosts' caller getter returns rc0,
value0, length4, expected111; errno107 is stale from getpeername. No guest
executes. Raw logs: `tcp-408642-{oracle,host}{2022,2025}.log`.

Thus WSAPoll did not consume the error in the isolated native control.
Winsock duplicates have separate connect-error ownership, and its SO_ERROR
getter is persistent rather than caller-consumptive. Pending source makes
POSIX aliases share the same native socket, with descriptor references counted
under the existing fd-table mutex and actual close only on the last descriptor.
Metadata snapshots do not extend the native handle lifetime. Existing
DescriptorGuard/ClosingDescriptor transactions are retained. Caller SO_ERROR
reads serialize and consume a failed-connect error once in shared metadata;
poll reads only that metadata to suppress the already-consumed native ERR.
It never calls the native error getter. A fresh connect resets ownership.

New strict reference controls preserve a source after dup2/source-close, keep
the old target alive through its remaining alias, require peer EOF at each
last-close, preserve urgent waits across sibling-close, and preserve UDP
alias payload. Replacing a failed fd with a fresh socket must report a new
unconsumed refusal. These and the implementation remain source-only until
Linux reference, actual both-ABI component and existing supplier/full gates.
Shared O_NONBLOCK/reuse cache flags, general connected-socket asynchronous
error consumption, failed-socket TCP reconnect, AF_UNSPEC and concurrent
multiple-connect semantics remain unqualified; no complete open-description
or close/poll concurrency parity is claimed.

## Unpublished OFD review checkpoint

Local WIP e4982cf0 was not published or compiled: independent peers rejected
the generic consumed-error boolean, registration reservation/publication gap,
stale UpdateSocket replacement, copied alias status flags and pre-success
dup2 target closure. Do not promote that checkpoint or infer acceptance.

Separate approved-base diagnostic17007c81 on `wip/net-tcp-error-state` passed
characterization40868155575. Both actual Windows verify INET/STREAM/TCP but
SIO_TCP_INFO after refusal fails with10045 and returned0; its zeroed State is
not a positive CLOSED observation. Send fails10057 and leaves SO_ERROR10061
(or0 after the original connecting alias closed). Original-handle retries
produce a new pending refusal; the closed-connecting-alias case returns10022.
Raw logs: `tcp-408681-win{2022,2025}.log`. No unsupported output is used as
a connection-state predicate.

The revised local source holds DescriptorGuard across RegisterSocket's full
reserve/publish path. It removes UpdateSocket, shares nonblocking/reuse flags,
and serializes native mode updates with descriptor identity checks. dup2
keeps the old native target until CRT success and swaps old/new descriptions
under one table lock. Initialization normalizes both blocking directions,
including accept's non-inherited O_NONBLOCK behavior.

The error-token hypothesis now verifies the native TCP provider, tracks each
connect generation, and always calls the native getter when the caller asks.
Only the same caller-observed ECONNREFUSED of the same failed generation may
be reported as consumed; different native codes remain visible. Proposed poll
filtering additionally requires the observed native ERR|HUP|WRNORM signature
and actual peer-not-connected state. New connect calls reset the token before
Winsock; overlapping native connect invocations are rejected as EALREADY.
The terminal-refusal API boundary still needs independent peer approval and
actual regression evidence; neither native HUP nor unsupported TCP_INFO alone
is accepted as generic CLOSED proof.

Source-only regressions add both alias directions of F_SETFL/FIONBIO/reuse,
bounded MSG_DONTWAIT, invalid-source dup2 preserving its target, UDP binding
retained until last close then released, post-refusal send, concurrent caller
getters with exactly one error, and independent refusal after failed-fd
replacement. system.cc joins exact compiler-role evidence. New Linux-first,
all actual Windows paths and original supplier/full gates are still required.

The next local revision addresses the getter-during-native-connect review
finding by restricting virtualization to verified nonblocking attempts.
DescriptorGuard and the error mutex remain held through that demonstrably
nonblocking native call and metadata finalization; F_SETFL/FIONBIO use the
same guard, so the mode cannot change mid-call. Blocking connect releases
those locks before waiting and is explicitly ineligible for the new refusal
token. Generic blocking-connect error ownership remains unqualified.

An internal ConnectWithObserver operation dependency is shared by the POSIX
entry (always a null observer) and the host-only regression. It has no mutable
global callback or guest symbol registration. A real post-native barrier waits
for native refusal without reading SO_ERROR, holds finalization, and starts a
real caller; another thread probes the exact metadata mutex before releasing
the gap. The final observation requires actual refusal111 then0. Linux and
both NDK contexts run the corresponding public-call barrier and concurrent
getter controls. Compiler evidence distinguishes the host GESHER compilation;
the producer also rejects private observer exports in all three native output
roles. These controls and the exact-code/generation terminal-refusal boundary
remain source-only pending independent peer and actual CI.

Fresh peer rejected the b9cd4698 mode boundary: the unlocked blocking branch
can race F_SETFL/FIONBIO before the native call and become nonblocking while
its refusal token remains ineligible. b9cd4698 is not publishable or runnable
as a product candidate. All local checkpoints remain preserved and unqualified.
The next independent native hypothesis is ConnectEx overlapped submission,
which must be characterized for both socket modes, mode changes, explicit
bind, context/options, refusal/error ownership, originating-thread exit and
cancel/close completion lifetime before any production rewrite. No mode flip,
unsupported TCP_INFO result, borrowed OVERLAPPED lifetime or global lock over
a blocking native wait is an accepted substitute.

### Owned-connect implementation checkpoint (unpublished, unqualified)

Independent a3dce5b8/408870 and 2c655b0a/408966 actual logs establish a
separate ConnectEx error channel: all eight mode/bind/options cases pass;
refusal completion10061 coexists with caller native SO_ERROR0/0. BOOL getters
write one byte for KEEPALIVE/NODELAY (timeout writes four); this remains a
separate Linux-ABI copying gap. Initiating-thread exit cancels native I/O995;
an owner which remains live completes refusal10061 and success0 after both
requesters actually exit. The same owner drains both operations before stop.
All13 latest submissions queued997 in0ms; both Windows have28 poll snapshots.
Pending raw combined interest is0, completed refusal/cancel is19+exception1,
success is16+exception0. Band-only is0 throughout. Reading the owned result or
native caller SO_ERROR does not remove raw failed-connect readiness. Raw logs:
`C:/work/gesher-ci-review/tcp-408966-{win2022,win2025,linux}.log`.

The local production rewrite uses a worker per pending TCP connect, retained
through actual native completion. It owns copied address/OVERLAPPED/events and
a containing-DLL reference; it destroys context before FreeLibraryAndExitThread.
Logical mode and generation publication occur under DescriptorGuard, while
submission/completion waits run outside it. ConnectEx never changes FIONBIO.
Shared error_mutex orders native submit/query/last-close; after close, completion
never queries a possibly reused SOCKET number. Generation updates target the
original shared description, never a looked-up numeric fd. Pending operations
are excluded from native poll until context publication; finite10ms slices
retain the caller deadline. Close/replacement still cancel the actual endpoint;
the worker retains context until its event proves completion. Failed waits retain
the owner rather than abandoning native context. Deferred cancellation requests
target the exact operation and leave its owner to drain. SO_SNDTIMEO returns
EINPROGRESS without freeing a live operation. These paths are source-only.

Every caller SO_ERROR still reads the native channel, including an equal later
code. A separate owned completion token is consumed only after successful guest
copy. This removes generic native-code masking. The poll rule is narrower but
still BLOCKED pending semantic peer and negative evidence: only a positively
completed ConnectEx refusal which never established a transport can classify
exact raw ERR|HUP|WRNORM residue; a new connect invalidates classification before
submission. No claim that this alone excludes every later independent native
error is accepted yet. Successful transports, other errors, non-TCP/AF_UNSPEC,
generic select semantics and inline urgent remain outside this proposed rule.

Direct source regressions retain all prior alias/UDP/last-close/fault guards and
add requester-exit, cancellation drain across exact fd reuse, and post-refusal
normal/OOB/sendto/shutdown controls. The old metadata-lock barrier now requires
the actual getter to proceed while the requester is paused: finalization is owned
independently. Still required before product publication: fresh independent full
source peer, native terminal-error boundary controls, module/DLL unload and
pending-cancellation/timeout actual controls, Linux reference and both actual
Windows host/bionic ABI paths with current typed source/compiler/member receipts.
No product CI has run for this rewrite and none of these source changes qualify it.

The next local revision returns the real owner through pthread's normal wrapper,
including its TLS/CRT retirement. A pre-reserved WinAPI-only reaper holds a
noninherited DuplicateHandle of the actual self thread object, observes native
exit, closes lifetime records, then releases a DLL reference with
FreeLibraryAndExitThread. No numeric TID grants authority. The reaper/ticket/pin
exist before owner creation; creation failure publishes no-owner cleanup, while
failed actual-lifetime evidence parks the reaper and retains its owners. A new
private read-only retirement wait lets HOST verify the actual reaper exit; both
private helpers are forbidden PE exports in every producer role. This remains
unqualified for MinGW until actual code executes in its compiler/provider roles.

Native c91a9f64/409044 establishes a PRE-EXISTING general reset getter gap:
both ordinary connect and ConnectEx, each with getter-only/select/WSAPoll on
fresh endpoints, return SO_ERROR0/0 while a subsequent native caller MSG_PEEK
reports10054. Select has read+write mask3, combined poll19. There is no evidence
that WSAPoll consumed that error or that ConnectEx alone introduced the gap.
The original strict native10054 getter assertion remains RED and OPEN; no
production readiness peek or accepted-zero substitute is introduced. Independent
MSVC module jobs pass warmup+eight cycles each on both Windows, with exactly one
TLS/context retirement, true owner/reaper exit0, real module unload, invariant
handle baseline72/77, and exact numeric SOCKET reuse in all four cancellation
cases per Windows. Those contexts produce995; normal refusal produces10061.
This proves the independent MSVC pattern, not MinGW product DLL equivalence.

The proposed bounded refusal rule now requires verified immutable INET/TCP
provider identity, exact completed refusal generation equal to current generation,
connect_result111, Failed phase, no live owned operation, no legacy native
connect/disconnect call and the observed exact native19 residue. The caller's
native SO_ERROR is never suppressed. Every owned or legacy connect entry
serializes attempt publication and invalidates the prior generation before
native submission; AF_UNSPEC remains a native, unqualified path, never a source
of the refusal token. Native failure preserves prior phase rather than inventing
a successful disconnection. An actual successful AF_UNSPEC transition would
return to Initial. New connection attempts cannot overlap a legacy native call.
Closed/reused descriptors use a different shared description; late completion
cannot target it. Terminal send/OOB/sendto/shutdown have no established transport
and no data I/O in flight; their actual native calls all fail10057/SO_ERROR0.
The bounded rule still requires independent whole-source semantic disposition.

The new HOST-only controls invoke the same actual production operation dependency,
then change native mode in BOTH directions after proven pending submission,
cancel the real logical blocking wait, and exercise SO_SNDTIMEO60ms without
abandoning I/O. All wait for actual owner/reaper exit. The requester-finalization
test requires a real getter to complete BEFORE the paused requester resumes;
a transient try-lock result is only an observation. Portable negative controls
require established reset POLLERR to remain visible and an explicit caller recv
to receive104, then consume it. They do not weaken the separate pre-receive
SO_ERROR104 assertion. All previous Linux/UDP/alias/last-close controls remain.
Source, cancellation, timeout, module and all three actual product contexts are
still pending; no product CI or qualification has occurred for this checkpoint.

Linux-only18df54eb/409094 is actual GREEN. A real zero-backlog listener and
accepted-queue filler prove a pending kernel attempt. SO_SNDTIMEO60ms returns
EINPROGRESS at62ms with poll0/error0; the SAME socket completes with writable4,
error0 and payload at1045ms. Deferred pthread cancellation really exits the
blocked caller with PTHREAD_CANCELED, but leaves its socket pending/error0;
the SAME attempt later completes and carries payload at1024ms. Both mode
transitions preserve the original call behavior and later transport. Full
original Linux inline/noninline reference remains GREEN. Raw actual log:
`C:/work/gesher-ci-review/tcp-409094-linux.log`.

This disproves9bed's targeted CancelIoEx on caller cancellation. The new local
revision removes that cancellation; only last descriptor close cancels native
I/O. HOST now requires the later actual refusal111 after canceled waiter or
send timeout, then real owning pthread and reaper exit. A read-only WinSDK
handle/thread census compares repeated operations after every retained object
is released; private census/retirement/observer helpers are forbidden exports.
The census and cancellation paths still need actual MinGW execution.

All socket poll samples now use the original deadline in10ms slices, with exact
fd-to-description validation after each. A new private immutable dependency
pauses the REAL production poll after its snapshots, allowing controlled last
close/reuse and dup2 replacement with a surviving alias plus simultaneous UDP
readiness. The required result rejects the stale identity as NVAL while counting
every other ready entry, preserves urgent bytes on old/replacement transports,
and retains the original timeout. Count-zero finite poll is separately retained.
The public poll path uses the same implementation with a null observer.

Finite polling also must not wait for SO_LINGER while refreshing metadata.
Native close now publishes a short closing state, performs closesocket without
holding the description mutex, then publishes its result. Pending native context
remains owned while close is active; no query can use a recycled SOCKET. The
owner/caller waits without spinning until actual completion and close publication.
Legacy/AF_UNSPEC raw-call close/reuse ordering is still a source-review BLOCKER;
no source pass or product publication is implied by these corrections.

Independent faadc255/409147 is actual GREEN on Linux and both Windows. The
observed Windows IPv4 base provider has chain1 and GUID
`e70f1aa0-ab8b-11cf-8ca3-00805f48a192` (TCP flags00020066, UDP00020609).
All ten measured Windows AF_UNSPEC calls take0ms, but this is not a universal
fast-call guarantee. Idle/ordinary-refused/owned-refused TCP return10047;
established TCP returns10056 and remains connected; UDP returns0 and becomes
disconnected while retaining its local port. Linux all five calls return0;
established TCP disconnect reports104 then0, UDP releases its local port.
Disconnect equivalence remains OPEN. Raw log:
`C:/work/gesher-ci-review/tcp-409147-legacy-all.log`.

The next proposed source bounds owned-connect behavior to that exact base
MSAFD IPv4 TCP provider, full protocol-info result, chain1 and AF_INET request.
IPv6 and other providers retain the unqualified legacy behavior. Producer and
consumer receipts now carry the exact owned-connect provider scope. The same
qualified base UDP and TCP AF_UNSPEC controls use native_call_mutex to coordinate
their actual handle against physical close. Poll/Refresh/SO_ERROR never acquire
that lease. No completion-metadata or descriptor-global lock covers the raw
legacy control call; there is no assumption that every provider call is instant.

Normal last-close and replaced dup2-target CRT/table transactions finish and
release the global descriptor guard BEFORE physical closesocket or waiting for
the native lease. Native-closing intent is published before fd reuse; both old
description and actual handle remain owned through physical close. Close returns
its native outcome. dup2 retains POSIX's successful replacement despite an old
close error, but first records failed native ownership in a self-retaining
description quarantine without allocating after table removal. Unpublished
rollback retains its original private CRT-slot preservation policy.

The new HOST barrier holds the REAL legacy lease before native entry, then
performs last-close/forced fd reuse (idle TCP, UDP and prior-owned-refusal TCP)
or actual dup2-target replacement. New descriptor operations and the replacement
getter must progress while the old physical closer remains blocked; resuming the
old call must return EBADF and cannot touch the replacement. Physical close and
both caller threads must then retire. Bind/listen invalidate old refusal
classification before mutation; a portable behavioral control turns a refused
socket into an empty listener, requires its full finite PRI timeout, then accepts
a NEW transport and urgent byte. No metadata label substitutes for native success.

A separate HOST WinSDK fixture creates a real UDP ICMP error with reset reporting
explicitly enabled, proves native POLLERR before mapping, then passes that actual
socket through production RegisterSocket/poll/recv. It requires error visibility
and caller receive104 outside the TCP-only filter. This does not change or qualify
the product's pre-existing UDP reset-reporting policy. The fixture is a separate
required host compiler source (nine total selected host sources); its helper,
legacy callback, poll callback, census and retirement helpers are forbidden PE
exports in all three native roles. Existing wrong-role/import/archive/source,
configured compiler and both-ABI evidence requirements remain strict.

All changes above remain UNPUBLISHED pending independent whole-source review and
fresh actual compiler/host/both-ABI contexts. The general established SO_ERROR104
getter failure stays strict OPEN; arbitrary providers/IPv6, TCP disconnect
equivalence, generic select, inline OOB and MinGW DLL-unload equivalence are not
closed by this bounded candidate. Local checks are source-only AST/format review;
no native build or fixture has executed locally.

Source ownership audit caught a native-argument lifetime gap before publication:
the owned neutral address had been converted into a temporary worker-block
sockaddr. b407f179/16223bcc move the ACTUAL native sockaddr_storage and length
into ConnectOperation and pass nullptr for the unused optional ConnectEx byte
output. No pending native argument now borrows a caller or worker local; this
matches the independently qualified native operation shape.

Linux-only a21c5c3a/409301 confirms the exact listener precondition. Fresh and
blocking-refused sockets reject a conflicting bind98, then listen succeeds with
finite PRI60ms and actual new accepted payload. Nonblocking-refused returns
listen EINVAL22 even after caller SO_ERROR111 consumption, retaining HUP16.
That matches Linux's distinct user socket state. The new positive transition
fixture now uses the PROVEN blocking-refused state; every pre-existing strict
nonblocking refusal/alias/error/deadline control remains unchanged. Raw log:
`C:/work/gesher-ci-review/tcp-409301-linux-listen.log`. Nonblocking-failed listen,
like TCP AF_UNSPEC disconnect, is not claimed equivalent by this candidate.

Independent whole-source review found a real all-invalid WSAPoll race before
publication. Closing the sole sampled native endpoint after lifecycle capture
but before the native call can yield global WSAEINVAL, bypassing fd identity
revalidation and discarding simultaneous eventfd readiness. The candidate now
recovers ONLY that native error and ONLY positively closing/closed original
descriptions. Each zero-time retry removes at least one retired endpoint; other
native errors and WSAEINVAL with no retired description remain failures. The
single caller deadline is never restarted.

An immutable private dependency pauses the REAL finalized WSAPoll inputs and
records its actual result. Required HOST controls force a sole closed socket,
that same all-invalid native array beside a ready eventfd with exact fd reuse,
and an independent ready UDP entry. They require NVAL on the original identity,
all other readiness and payload preservation, and a genuine unowned invalid
native input still failing EINVAL. The first two cases require actual native
WSAEINVAL10022, not a fabricated wrapper result. These new source controls remain
UNQUALIFIED until Linux-first and fresh HOST/both-ABI CI after independent peer.

Whole-source peer conditionally passed 423f123b; its first current-source
characterization409352 failed Linux compilation at the new listener control's
misspelled helper name (`finite_priority_wait` versus existing
`finite_priority_control`). Both independent WinSDK jobs passed, with no product
component triggered. Correcting that call preserves the existing strict 60ms
helper and every assertion/bound. Raw compile log:
`C:/work/gesher-ci-review/tcp-409352-linux.log`.

Corrected-helper8f185c13/409358 compiles. Actual Linux passes both inline and
noninline original scenarios, finite waits, strict refusal/error consumption,
requester reuse, reset receive and the new listener transition, then fails the
new urgent-alias last-close EOF control with ECONNRESET104. No Windows product
component was triggered. Strict EOF remained unchanged while independent Linux
92f73439/409371 isolated four fresh transports: ordinary consumed input closes
with recv0; OOB-only consumption then close produces recv-1/104; adding sender
SHUT_WR and real normal-stream receive0 leaves peer EAGAIN11 until receiver's
last close, then recv0, including a surviving dup alias. Linger was actually0.

The product fixture now establishes that proven orderly-close precondition,
requires real stream0 plus peer EAGAIN BEFORE last close, and retains strict
peer EOF0 afterward. All three last-alias checks print named actual poll/read
results; every consumer context requires those EOF and stream-drain markers.
This is a fixture ordering correction, not reset acceptance or production error
normalization. Original strict reset/SO_ERROR gaps remain OPEN. Raw logs:
`C:/work/gesher-ci-review/tcp-409358-linux.log`,
`C:/work/gesher-ci-review/tcp-409371-linux-alias-close.log`.

Current-source c3347dc8/409376 characterization is terminal GREEN. Direct Linux
raw audit confirms both original inline/noninline scenarios, finite60ms endpoint
waits, freshHUP16, strict refusal24/caller111 then0/HUP16, concurrent error
consumption, requester/reuse/reset controls, proven blocking-failed listener
transition, all three named strict alias EOF0 phases, normal urgent-stream drain0
and peer EAGAIN11 before last close, shared flags/UDP payload, delayed arrival and
the complete-contract final marker. Both independent WinSDK jobs also succeeded.
Raw current Linux log: `C:/work/gesher-ci-review/tcp-409376-linux.log`.

The next comment-only workflow change requests one fresh component build and
actual HOST/x86_64/arm64 contexts on both Windows versions, after independent
trigger peer. No production source or assertion changes accompany that trigger.
The real owned-mode/cancel/SNDTIMEO/owner-retirement/census, finalized-native-poll
race, legacy lease, UDP error and all compiler/archive/member/import/source-role
gates remain mandatory. No Windows product path has yet qualified this source.

First current component8142c127/409385 is RED at compilation, with Linux full
reference PASS and Windows contexts skipped. The product's changed socket/fd/
system/winsock objects and HOST contract compiled, but the separate WinSDK UDP
input fixture omitted `<mswsock.h>` and its SIO_UDP_CONNRESET declaration was
therefore missing in the actual pinned llvm-mingw compiler. The correction adds
that canonical header, already included by the successfully compiled production
winsock.cc, without numeric substitutes or behavior/assertion changes. A comment
triggers one fresh changed-source component after independent peer. Raw log:
`C:/work/gesher-ci-review/tcp-409385-compile.log`. No runtime inference follows
from compiler success and no Windows product path is yet qualified.

Corrected9bcfd57a/409406 producer and Linux reference PASS. Actual producer binds
all nine selected HOST sources, translator source, both freshly compiled NDK
ABIs, exact publisher archive and native import/private-export controls. Both
Windows consumers authenticated archive12927936155/digest3cad6dd8231274e437784c43b9c06c68c97f0345e31f7ed8fe5632caec25255a
from compile123682982568, source9bcfd57a/run40940693211/attempt1 and strict source/
compiler/member/import roles before actual HOST execution. No guest ABI ran.

Actual HOST on BOTH Windows passes noninline urgent, finite62ms endpoint waits,
freshHUP16, refused24 then caller111/0 and HUP16, concurrent error getters,
getter-before-requester-release, requester exit/fd reuse and established recv104.
Owned mode0 retires with handles137/137 (2022),146/146 (2025), threads4/4; mode1
retires with one extra handle138/137 and147/146, threads4/4, triggering the
UNCHANGED strict census. Cancellation mode2 was not reached, so this failure
cannot be attributed to cancellation or used as cancellation evidence.

The next diagnostic changes only fixture telemetry: weak operation/description
reference counts after caller release and a bounded read-only handle/type snapshot
of THIS process before the unchanged census assertion. It creates no retained
aliases, queries no file/pipe object names and validates returned native lengths.
Raw observed handle values are telemetry, not reusable identity authority. The
diagnostic itself must preserve process handle count, and its private helper is
forbidden in every PE export role. Production source, all original assertions,
timeouts and the strict process handle/thread bound remain unchanged. Logs:
`C:/work/gesher-ci-review/tcp-409406-compile.log`,
`C:/work/gesher-ci-review/tcp-409406-win2022.log`,
`C:/work/gesher-ci-review/tcp-409406-win2025.log`.

Actual compiler commands include global -fno-exceptions. Upstream winpthreads
walks its explicit cleanup chain and exits via longjmp; automatic C++ shared_ptr
unwind must not be assumed. This remains an independent source/actual-cancellation
qualification condition, not the cause of the earlier mode1 failure. Reference:
https://github.com/mingw-w64/mingw-w64/blob/master/mingw-w64-libraries/winpthreads/src/thread.c

b59e6f82/409453 actual telemetry reaches the unchanged mode1 bound and fails it
on both Windows. Mode0/1 weak operation and description strong counts are0.
Own handle snapshots succeed and their before/after counts are identical:
137/138 (2022),146/147 (2025). Type counts change only Event33 to34, with one
new observed raw value576; all other observed entries match. This does NOT prove
that value's object identity, creator, caching behavior or leak cause. Mode2 and
both guest ABIs remain unexecuted. Raw logs:
`C:/work/gesher-ci-review/tcp-409453-win2022.log`,
`C:/work/gesher-ci-review/tcp-409453-win2025.log`.

The next bounded diagnostic enables kernel handle tracing for THIS fixture
process before mode0, requires successful enable without changing handle count,
and queries OPEN/CLOSE history only for newly observed local Event values.
Native returned lengths/counts are validated. Only mapped user module basenames
and RVAs are printed, not raw kernel addresses; process/thread IDs are telemetry,
never authorities for opening another process/thread. Private retirement evidence
also logs the actual three owned operation handles and each CloseHandle result.
The WinAPI-only reaper gains no CRT/logging calls. Production completion behavior,
native socket ownership and the original exact census remain unchanged. No +1
allowance or larger/warmed baseline is introduced. Whether caller-side native
completion query creates a provider event is only a hypothesis until actual
creator-stack evidence, not justification for a source or assertion change.

a0d62ee7/409490 actual tracing succeeds on both Windows with unchanged counts.
All three explicitly owned operation handles close successfully (result1/error0)
and operation/description strong refs are0. The extra Event has one kernel OPEN
record whose user frames are KERNELBASE then probe.exe RVAs0x46225,0x21768,0x2247f,
then ucrtbase/Kernel32/ntdll, identical probe RVAs on both OS versions. No WS2_32
frame establishes the earlier provider-query hypothesis. The unmodified strict
mode1 census still fails and mode2/guest ABIs remain unexecuted. Raw logs:
`C:/work/gesher-ci-review/tcp-409490-win2022.log`,
`C:/work/gesher-ci-review/tcp-409490-win2025.log`.

The next diagnostic prints RtlLookupFunctionEntry ranges for actual stack PCs
and known same-binary requester/observer/pthread functions, plus the exact owning
pthread/reaper code RVAs from private retirement evidence. It neither downloads
an executable locally nor guesses function names from another binary's offsets.
No completion, lifetime, callback, cleanup or strict-census behavior changes.

Range telemetry also requires the unwind API's function image to equal the
independently mapped PC module before printing ranges. Every known/trace/owner
code row includes its module identity; an unmapped leaf is explicitly labeled
range_valid0. Cross-module equal RVA numbers cannot establish a function match.
