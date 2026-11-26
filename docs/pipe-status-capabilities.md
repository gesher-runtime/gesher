# Windows pipe status and supported operations

Gesher uses real, synchronous Windows message-type pipes with byte-stream
reads. Windows owns byte queues, capacity, pending requests and peer lifetime.
The runtime does not keep a second byte queue, estimate peer counts, or retain
the opposite endpoint to manufacture readiness. Ordinary native children keep
their supported synchronous handles.

The file-description work in [PR #73](https://github.com/gesher-runtime/gesher/pull/73)
remains. This repair also retains the valid goals and diagnostic evidence
contributed in [PR #68](https://github.com/gesher-runtime/gesher/pull/68).
The limits below are explicit failures of legal Linux operations, not a claim
of full concurrent pipe compatibility.

## Ownership and status

Each runtime-created endpoint has actual access rights, a shared metadata
section and a named admission mutex. Duplication and aware-process transport
share the description. Metadata retains no extra pipe endpoint handle.

Adoption opens or recreates the mutex named by the validated shared nonce.
The sender may release its last description after process creation or broker
transfer, before the receiver imports it. An active operation retains its
description and mutex handle; a missing named mutex consequently has no active
owner. Recreating that same name preserves the shared state and avoids a child
startup failure when its sender has already retired the last local handle.

The creator records the actual initial wait mode before publication. `F_GETFL`
reads known status without entering a kernel query. `F_SETFL` changes the real
kernel mode before publishing `O_NONBLOCK`. Shared metadata records an active
transition: if its owner dies before completion, potentially stale status is
reported as unavailable. Readiness and byte-count queries acquire admission
without waiting and read actual `FilePipeLocalInformation`.

An aware process re-reads the shared flags after obtaining transition
admission. Unchanged-mode and metadata-only updates compare-exchange the full
observed flags and retry if another process changed them. A delayed caller
cannot overwrite a completed native mode change with stale metadata, while
no-op and metadata-only updates remain usable during an admitted transfer.

| App request | Behavior and explicit capability limit |
| --- | --- |
| Pipe `read`/`write` | Real bytes, EOF and peer errors. Blocking writes wait for actual reader progress. |
| Nonblocking write at most `PIPE_BUF` (4096), including vectors | Complete message admission or `EAGAIN` without adding bytes. |
| Larger nonblocking write | Positive partial results are allowed. Quota is only an admission hint; the actual write decides success or error. |
| `F_GETFL` during an owned read/write | Returns known cached status without waiting for that transfer. |
| Changing `O_NONBLOCK` on an idle endpoint | Succeeds only after the actual transition succeeds. NPFS `ERROR_PIPE_BUSY`, including with buffered bytes, becomes `EOPNOTSUPP` with flags unchanged. |
| Changing `O_NONBLOCK` during endpoint I/O in this or another aware process | Returns `EOPNOTSUPP`. Unchanged wait policy and metadata-only flags need no kernel transition. |
| `poll`/`select`/`epoll` on an owned idle endpoint | Uses actual queued bytes, quota and peer state. An active endpoint returns operation error `EOPNOTSUPP`, never `POLLNVAL` or invented readiness. |
| Reader-side `FIONREAD` | Actual queued bytes when owned and idle. Active/unmediated endpoints return `EOPNOTSUPP`, leaving output untouched. |
| Writer-side `FIONREAD` | `EOPNOTSUPP`: quota subtraction is not a valid queue-length measurement. |
| Pipe `FIONBIO` | Same real transition and failure policy as `F_SETFL`. |
| Fresh proc-fd open for pipes or another unsupported non-disk object | `EOPNOTSUPP`; no substitution of `dup`, changed direction, forced `FD_CLOEXEC`, or shared status masquerading as a fresh open. |
| `dup`, `dup2`, snapshots and aware inheritance | Keep shared status and the real endpoint, with independent descriptor flags as appropriate. |

Native export reserves known pipe policy through `CreateProcess`. Failure
restores ownership; success marks the endpoint unmediated. A native program
can change modes or pass a handle onward. Status changes and readiness/count
queries therefore remain unsupported for that endpoint's surviving aliases,
including after the first child exits. Local child counts cannot prove that
all native references or changes are gone. Known nonblocking native export is
rejected instead of silently losing its policy.

Raw external pipes supply actual access rights, not invented status. Real
byte I/O and direct native export remain available; full status and readiness
queries fail explicitly. I/O supplies a unique valid `OVERLAPPED` for a
potentially blocking or external operation and handles pending completion.
It does not assume arbitrary native `ReadFile(..., NULL)` works on async
handles. Native APIs exposing an owned pipe must use the export reservation;
direct Win32 use bypassing that boundary is not exclusively mediated I/O.

## Cancellation and resources

Known immediate nonblocking I/O executes directly. Blocking I/O also runs on
the calling thread, without a dedicated worker or new 64 KiB stack per call.
A Windows thread-pool wait invokes a callback only on a winpthreads
cancellation event or an explicitly supplied interruption event. It cancels
the exact async request and/or pinned calling
thread's synchronous request, retrying the pre-entry race. Every callback is
joined before buffers, events, thread references or admission are released.

Cancellation delivery is disabled internally until native retirement and
cleanup finish. An already disabled caller gets no callback: its I/O completes
normally and its pending cancellation remains available after reenable.

Shell pipeline and `popen` creators initialize their fresh pipe descriptions
before publication. Shell reads validate access separately from public status
availability, then read actual bytes through the same native transfer path.
One shell mailbox event can interrupt an idle external pipe read so its trap
runs on the shell thread; a non-interruptible command read handles the trap
and resumes. The event has process lifetime and is noninheritable. Each read
retires its wait callbacks before that event is reset or its buffer is reused.
Unknown external wait mode is never converted into invented `O_NONBLOCK`, EOF,
or success, and no native mode query is issued behind pending pipe I/O.

## Native evidence and retained PR #68 coverage

- PR #68's observer at `d9fd890417831dd66a1eeb62e5618fc67dc97516`
  demonstrated useful real transfers, message admission and native children.
  Its seven failed assumptions are evidence, not passing product contracts.
  Pending reads reserve writer quota; last-reader closure can discard bytes.
- [Run 44309820222](https://github.com/gesher-runtime/gesher/actions/runs/44309820222),
  observer `2f9e077e5e6360759863eac637d4029a6d38bafb`, on Windows 2022/2025:
  all twelve `ReOpenFile` variants failed 231; synchronous pending-read queries
  exceeded their owned-child deadline; overlapped queries worked, but changing
  mode during a pending read still failed 231.
- [Run 44317117523](https://github.com/gesher-runtime/gesher/actions/runs/44317117523),
  observer `7133e0d0b3571ed88765f9a2a16530d3017ce313`: both Windows hosts
  rejected `FSCTL_PIPE_ASSIGN_EVENT` with `ERROR_INVALID_FUNCTION`.

| Useful PR #68 scenario | Retained regression |
| --- | --- |
| Atomic small nonblocking admission | `pipe_kernel_test`: partial drain, scalar/vector `EAGAIN`, unchanged reader byte count. |
| Full blocking write with a real reader | `pipe_kernel_test`: exact 65,537-byte write and drain. |
| Competing readers and real EOF | `pipe_kernel_test`: one winner/one `EAGAIN`, real last-writer closure and HUP. |
| Native peer survives local alias closure | `ofd_spawn_test` and pipe component process fixture: directional input/output, genuine peer lifetime and EOF. |
| Writer byte-count accuracy | `pipe_kernel_test`: explicit unsupported result, unchanged output, no quota subtraction. |
| Shared status across aliases/aware children | `file_flags_ofd_test`, `ofd_spawn_test`, production OFD/exec host suite. |

The pipe fixture additionally checks active-query errors, rejected transitions,
idle changes, cancellation retirement, disabled cancellation, descriptor
reuse and unsupported proc-fd opens. Linux references and host boundaries do
not substitute for executing the production component on Windows.

Primary contracts:
[ReadFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile),
[pipe modes](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-type-read-and-wait-modes),
[CancelSynchronousIo](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelsynchronousio),
[wait callback retirement](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-waitforthreadpoolwaitcallbacks),
[winpthreads cancellation](https://github.com/mingw-w64/mingw-w64/blob/master/mingw-w64-libraries/winpthreads/src/thread.c).
