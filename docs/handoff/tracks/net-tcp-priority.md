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
