# PR73 repair candidate: native evidence still required

This candidate preserves PR73's open-description and shared-stdio work and
integrates main `e80cd1854163bd18e4ea5392c25f423184048804`. The only added
merge-queue workflow repair is the approved `.github/workflows` sparse-checkout
input. It is an intermediate repair, not an approval or evidence that Windows
or Android execution succeeded.

## Follow-up to published head f6121d0c

Current-head run `44174755368` stopped before the relevant Windows runtime
contracts. Two failures belong to this PR: the launcher still referenced
`ExitIfVforkChild`, and the Linux formatter envelope leaked its 10-byte
`vasprintf` allocation when the blocked output test canceled its thread.

The exit integration problem was not merely a missing declaration. PR73 had
removed both the declaration and implementation of the old synthetic-child
mechanism, but left two calls in the launcher. Those obsolete interceptions
are now removed. Normal `exit` still runs `__cxa_finalize` once, flushes stdio
and terminates the actual process with the low eight status bits. `_exit` and
`_Exit` still terminate directly, without finalization or flushing. Successful
exec/spawn paths retain their existing process transport. `fork`/`vfork` remain
unimplemented and return `ENOSYS`; this is a remaining runtime capability gap,
not a successful Android fork implementation. The documented exec process-id
limitation also remains.

The formatter envelope now owns its temporary narrow and wide buffers across
cancellation, and its scanner proxy owns its temporary FILE. Existing
cancellation inputs and all 125 adapter assertions remain unchanged. An
external allocation observer runs the same test against published and repaired
envelopes: the former leaves the actual formatted allocation outstanding, while
the latter calls its real deallocator after cancellation. The observer does
not replace LeakSanitizer. Local LeakSanitizer still cannot run under the
workspace's tracer, and CI keeps its normal sanitizer settings.

The complete launcher translation unit compiles on x64 and ARM64. The exact
owned exit function bodies also link in native probes and pass seven real
Linux subprocess cases covering finalization, buffered output, immediate exit,
status truncation and recursive exit. Full launcher linking and Windows/ART
execution remain CI requirements. Unrelated shared-tree build failures remain
an external dependency; this follow-up changes no shared-tree workaround or
test deadline. The pipe blockers below remain open, with their native probes
and assertions intact.

## Further CI evidence for published head 6aec0eb3

Run `44194596986`, artifact-preflight job `134436332958`, passed on the exact
published head `6aec0eb34ccc1d6735e003c7ba5da4faae710a40`. Its unchanged
`python3 tools/test_stdio_contracts.py --sanitize` command completed 269 core,
125 adapter and 66 Linux-reference checks, all four cancellation phases and
the exit-ordering case, with no leak report. This is new remote evidence for
that head only. It does not change the original local ptrace/LeakSanitizer
limitation or qualify the later lock/lifetime or fixture changes; a new
candidate still requires its own sanitizer CI.

The same run then exposed two PR-specific Android fixture build failures.
`pwritev` is declared by the NDK from API 24 under GNU feature selection, but
`exec2_child.c` did not select those declarations before including system
headers. The JNI route included `jni.h` before the shared driver selected
them, producing the same error when reproduced locally. Both translation
units now select `_GNU_SOURCE` before their first system header. The shared
process corpus and its real vector-write assertions remain unchanged.

The libc fixture intentionally targets API 26, while `fwrite_unlocked` was
introduced in API 28. The old-API branch now resolves that actual runtime
export with `dlsym`, just as other newer exports in the existing fixture do.
It still invokes the real unlocked operation while another thread holds the
stream lock. An absent export makes the existing assertion fail; there is no
locked fallback or skipped check. Windows, Linux and Android API 28+ retain
their direct call. All original stdio assertions and the API 26 target remain.

The four exact compile/link commands from the unchanged exec/native-libs
workflows were reproduced using original NDK r27c (`27.2.12479018`). The
publisher archive SHA-1 and compiler/header/library bytes were verified.
Before the repair the child and JNI commands fail on `pwritev`, and the API 26
libc command fails on `fwrite_unlocked`; the standalone exec parent already
passes. All four commands pass after the repair. Additional libc builds pass
for x86-64 API 28 and ARM64 API 26/28. ELF imports show the child's actual
`pwritev@LIBC_N`, API 26 dynamic lookup without an API 28 direct import, and
API 28's actual `fwrite_unlocked@LIBC_P` import. These are compile/link and
symbol observations, not Android or Windows execution.

The shared stdio corpus also recompiles with original strict LLVM-MinGW for
x64 and ARM64 after this fixture change. The portable fortified ASan/UBSan
stdio checks still pass 269/127/66 with local leak detection explicitly
disabled because of ptrace. The original CI sanitizer settings, native
contracts, deadlines and unrelated shared-main helper pins are unchanged.

## Follow-up: native status work outside descriptor publication

The published CI-only follow-up is `6aec0eb34ccc1d6735e003c7ba5da4faae710a40`.
This subsequent repair changes the native status call paths, with no native
runtime approval implied. Previously a pipe mode query could retain both the
global descriptor transaction and the OFD table mutex; `F_SETFL` retained the
transaction while calling `SetNamedPipeHandleState`. Lazy discovery during
capture, enumeration and duplication had the same problem.

Capture now publishes an uninitialized description and owns an exact native
duplicate before releasing the transaction. Actual native flag discovery
happens only when status is needed, under the description's own mutex. Explicit
registration and inherited mappings already carry initialized flags. Export
initializes a lazy description before sharing it, so it never exports invented
zero status. Failed discovery leaves the description uninitialized and returns
the actual error; a later operation may retry it.

Public `fcntl`, regular-file I/O, pipe polling, `fdopen`, positioned I/O and
`lseek` release publication locks before native status inspection. Duplication
does not query status merely to establish alias ownership. `fdopen` verifies
the captured description again before attaching a FILE, so concurrent numeric
fd reuse cannot redirect the attachment. Native pipe read/write routing now
uses one snapshot for validation and transfer; the explicit cancellation
cleanup owns that same snapshot. The original pipe transfer worker is unchanged.

A private independent probe was frozen against the original source before
candidate inspection. It renders the whole production OFD implementation and
the exact public `fcntl` body, with real Linux descriptors behind a declared
Win32/UCRT boundary. Its native query/update hooks stall while another thread
captures an unrelated descriptor and closes/reuses the original slot. The old
source fails 12 of 52 observations: eight native calls retain the descriptor
transaction and six retain the table mutex. The repaired source passes all
54 observations, with neither global lock held at any of its eight native
boundary calls. The two additional observations become reachable when reuse
can finish before the stalled call is released. Old-object bytes/status and
all retained handles are checked after completion. This proves the scoped
lock/lifetime change at that boundary, not Windows pipe scheduling.

Original LLVM-MinGW compilation passes seven production units and four native
fixtures on each architecture (22 objects). Portable ASan/UBSan checks pass:
OFD/exec 75; regular-file and exact pipe-route 57; stdio 269 core, 127 adapter,
66 Linux reference; and the positioned public/fault/concurrent/null-fault
corpus. The two additional stdio allocation failures cover the new snapshots
while retaining the original State and registry-node allocation failures. The
unchanged external formatter observer still sees the canceled allocation
released. Local LeakSanitizer success is not claimed. Source qualification is
repinned to this exact transition with all original assertions and five
mutation controls retained.

Microsoft documents serialization of I/O through a synchronous file object,
and `DuplicateHandle` retains the same object. Consequently this repair does
not establish same-pipe `F_GETFL`, `F_SETFL` or readiness progress behind a
blocked read. It also does not bound `CancelSynchronousIo` retirement: that API
does not wait for the canceled request to finish. The existing Windows gates
remain unconditional, including their 90-second process deadline, and all
procfs, NPFS, native-child, cross-process status and virtual-registry blockers
below remain unresolved.

## Repairs established locally

The wide stream implementation now owns its symbols without MinGW macro or
C++ inline collisions. The public wchar header also prevents MinGW's inline
`fwide` from bypassing Gesher's stream orientation. Both production architectures
compile with the pinned LLVM-MinGW toolchain at `-O2 -Wall -Wextra -Werror`.
The fortified stdio adapter checks its summary write result. Track45 expects
the eight substantive exec2 checks that remain after removal of fake fork
completion; it uses the same executable body as the JNI route.

FILE close and reopen now retire socket/event/binder/epoll ownership in the
same descriptor transaction that observes CRT closure/replacement. Native
socket close and linger happen after that transaction, including failed
reopens. A new public regression corpus uses actual sockets, duplicate aliases,
event counters, EOF and slot reuse. Portable lifecycle envelopes remain
explicitly limited to their declared FILE/CRT boundaries.

OFD allocation failures are caught at the owning implementation's boundary.
Snapshot control blocks are allocated before native duplication; enumeration
failure releases accumulated snapshots. The original flags/description survive
failed registration. The source-specific exception setting is confined to
the OFD implementation, as it already was for stdio allocation.

The newly cancellable native pipe calls also require explicit stream cleanup.
The pinned winpthreads cleanup chain now releases stream locks and state pins,
flush-all snapshots and formatting context even when the runtime does not
unwind C++ objects. Public regression cases cancel blocked reads, formatted
writes, flush-all and close, then acquire and reuse the same stream. These are
host winpthreads/POSIX lifecycle tests, not an added Android pthread_cancel
capability; bionic does not expose that operation.

## Pipe implementation and concrete remaining gates

`pipe2` now creates local Windows message-type pipes with byte-stream reads.
Published read/write handles have actual directional data access. The kernel
wait mode supplies O_NONBLOCK across duplicated/inherited FILE_OBJECT handles.
Small writes submit one message; insufficient nonblocking message capacity
returns EAGAIN without partial admission. Larger nonblocking writes consult
the actual kernel write quota. Readiness observes the real kernel queue and
peer state; no local peer counter, condition variable, retained opposite end,
or assumed capacity controls application behavior.

Synchronous handles preserve ordinary UCRT-child compatibility. Each active
transfer uses one joinable worker with a 64 KiB stack reservation; the caller
waits at cancellation points. Cancellation repeatedly issues
`CancelSynchronousIo` until that exact worker exits, covering cancellation
before syscall entry. Neither the I/O buffer nor its retained endpoint can be
released first. There is no detached queue of transfers or retained idle worker.
This is a real per-call thread cost, including ready nonblocking operations;
it needs Windows throughput/latency measurement before considering a pool or
another backend. Portable tests establish bounded retirement, not acceptable
Windows throughput.

The following remain mandatory and unresolved until actual Windows execution:

1. `pipe_kernel_test.exe` must pass full-pipe scalar/vector admission, competing
   readers, F_GETFL/F_SETFL while another read is blocked, cancellation handle
   retirement, close/reuse of a retained snapshot, and native kernel readiness.
   The global descriptor/table lock retention is repaired as described above.
   Synchronous Windows FILE_OBJECT serialization remains a particular risk for
   status queries and mode changes while a worker is blocked.
2. The existing `/proc/self/fd` pipe path still duplicates a descriptor. It does
   not implement Linux's independent description, requested access direction,
   or ordinary CLOEXEC default. This is a known source-level blocker, not just
   missing test evidence. The new test checks all two-source/three-access cases
   against the same byte queue. It also prints isolated native ReOpenFile
   observations to determine whether a real independent endpoint is available.
   `ReOpenFile` documents named-pipe client handles, but that does not prove
   reopening the server endpoint or changing direction on the same Linux byte
   queue. No unsupported error has been substituted for the Linux operation.
3. `ofd_spawn_test.exe` must prove actual UCRT `_read`/`_write` on the inherited
   synchronous endpoints, real last-peer EOF, and an aware child's F_SETFL
   change through a high descriptor alias reaching its parent.
4. The public stdio socket/event regression must run on Windows, including
   failed reopen, numeric fd reuse and all four blocked-stdio cancellation
   cases. Host envelopes do not prove the virtual registries, the native
   winpthreads cleanup path or MinGW/UCRT symbol ownership at link/runtime.
5. Native inherited byte-type pipes and the runtime's pre-existing asynchronous
   signal/SIGPIPE limitations are not certified by the message-pipe fixture.
   They must not be inferred to have full Linux behavior from these results.

The native test retains all failures as gates. In particular, the procfs
case is expected to expose the known missing implementation; no skipped
assertion or accepted failure is introduced.

## Source qualification and supplier preservation

`tools/net-tcp-priority/qualified/ofd-stream-source.patch` records the exact
fd/system transition from the previously qualified main source. The policy
test reverses that pinned patch, then executes every original baseline/hash
assertion. It additionally compares the socket ownership functions, socket
vector dispatch and complete native polling core byte-for-byte and pins all
newly coupled OFD/stdio/pipe implementation inputs. Five mutation controls
demonstrate rejection of ownership, dispatch, pipe behavior and patch changes.

The supplier source/patch identities, DLL import roles, controller execution
tail, fixture assertions and deadlines, and two-supplier workflow comparisons
remain enforced. Source qualification is not a runtime pass or merge approval;
the qualification records `native_runtime_approval: false`.

## Primary behavior references

- [AOSP Android16 stdio](https://github.com/aosp-mirror/platform_bionic/blob/android16-release/libc/stdio/stdio.cpp),
  initial review blob `a5f2f81f44614fa5c7742741dcdd1ffdfe37ac73`.
- [Linux pipe behavior](https://github.com/torvalds/linux/blob/master/fs/pipe.c),
  initial review blob `4c1122597a3c379169fa39955936b5233d47b955`.
  This is a behavior reference only; no Linux implementation code is copied.
- [Microsoft named-pipe modes](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-type-read-and-wait-modes).
- [Microsoft SetNamedPipeHandleState](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-setnamedpipehandlestate).
- [Microsoft synchronous file-object serialization](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile).
- [Microsoft DuplicateHandle object sharing](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle).
- [Microsoft ReOpenFile](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile).
- [Microsoft CancelSynchronousIo](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelsynchronousio).
- [Winpthreads cleanup records](https://github.com/mingw-w64/mingw-w64/blob/master/mingw-w64-libraries/winpthreads/include/pthread.h)
  (the actual implementation is compiled against the pinned toolchain header).
- [Bionic functionality status](https://android.googlesource.com/platform/bionic/+/2109f12c3b04d4023030ad12e17197c4a1c843ac/docs/status.md)
  documents the absence of pthread cancellation.
- [Bionic GNU vector-I/O declarations](https://android.googlesource.com/platform/bionic/+/bcfe3cf/libc/include/sys/uio.h)
  and [stdio API introduction declarations](https://android.googlesource.com/platform/bionic/+/249a7ae70e3e051d55696d5028f2c3600ab6a6f0/libc/include/stdio.h),
  checked against the actual r27c headers. The
  [Bionic libc export map](https://android.googlesource.com/platform/bionic/+/249a7ae70e3e051d55696d5028f2c3600ab6a6f0/libc/libc.map.txt)
  records `pwritev` under API 24 and `fwrite_unlocked` under API 28.
- [Android kernel fcntl file ownership](https://android.googlesource.com/kernel/common/+/dd2c0198a8365dcc3bb6aed22313d56088e3af55/fs/fcntl.c),
  blob `e871009f6c8895cdcb2787040e268247439b0e92`, and
  [proc-fd target resolution](https://android.googlesource.com/kernel/common/+/refs/tags/android13-5.15-2025-03_r5/fs/proc/fd.c),
  blob `913bef0d2a36c4ed0d49dfba1b18cac612cdbe5f`, are behavior references.
- [Android kernel fresh pipe-open behavior](https://android.googlesource.com/kernel/common/+/dd2c0198a8365dcc3bb6aed22313d56088e3af55/fs/pipe.c),
  blob `6c1a9b1db9076c8c1402b7368a25ca7d9a894c70`. Requested access attaches new
  reader/writer participation to the same pipe. No GPL implementation is copied.

The Linux differential corpus observes its actual kernel behavior independently;
the host pipe envelope uses real Linux pipes and native threads but explicitly
models Windows message-write return conventions. Neither is evidence that NPFS,
UCRT, or Android/ART passed.
