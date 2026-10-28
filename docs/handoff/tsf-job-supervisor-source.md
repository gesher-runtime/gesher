# TSF owned job/file supervisor: source checkpoint

This is a source-only correction of the controller at base
`9255a43ca521a6c59359d28766069928b71f992f`. This correction has not been compiled or run.
The historical component run `40751383586` established 74 assertions and zero
failures for the old controller; it does not qualify this replacement.

## Ownership and completion protocol

`TsfJobSupervisor.cs` creates a private unnamed Win32 job with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, without either breakaway permission.
It creates exclusive new local stdout/stderr files and a NUL input handle.
The STARTUPINFOEX handle list admits only these three inheritable handles;
the job, process, and thread handles are never inherited. It creates the
explicit executable with CREATE_SUSPENDED and CREATE_NO_WINDOW, retains the
actual returned process/thread handles, and records PID, birth time and image
while suspended. A thread snapshot must show exactly one root thread, job
assignment and IsProcessInJob must succeed, pre-resume accounting must be
one process, and ResumeThread must report exactly one previous suspension.
An assignment failure terminates only the still-suspended exact root handle.

The real probe has one absolute 45,000ms main deadline, including controller
setup. WaitForSingleObject always targets the returned root handle, with at
most the remaining main deadline. Output length is sampled between waits.
There are no pipes, asynchronous readers or Process.WaitForExit/Dispose calls.
Once main ends, one 5,000ms cleanup stopwatch covers termination, root reaping,
job-empty verification, handle closure, bounded file snapshots and deletion.
No stage resets that stopwatch. A still-populated owned job receives
TerminateJobObject. Root signaling and its exit code are recorded on its
original handle. Every captured process must signal on its retained handle;
root/thread and other captured process handles close before requiring job accounting
ActiveProcesses==0, so our references cannot impede that accounting transition.
The job remains queryable until that observation. Closing the job alone is
never accepted as successful cleanup. Errors or expiration make Success false.

Only a signaled root, observed empty job, identity reconciliation, and successful
retirement of every captured process and owned handle permit snapshots. Each
regular file is opened with FileShare.None and length-checked before
allocating or reading at most 1MiB. Owned files and the unique directory are
deleted with errors retained. Zero exit is necessary but insufficient:
the original FAIL, summary and five required assertion checks still run.
Both C++ files and their 74 assertions are unchanged. All artifact producer,
run/attempt/source/compiler/import/member/raw archive digest guards remain.

## Supported scope and limits

This controller supports the repository's controlled probe and CI workers,
running without an adversarial peer in the same account. Descendants created
normally belong to its job; they may retain the inherited regular-file writer
handles. Those descendants must disappear from job accounting before snapshots.
The implementation enumerates the job's actual PID list and opens each live
identity with query/synchronize rights, retaining that handle through signaling.
It never terminates by PID or queries a known-exited process image. Any failure
to capture identity while live rejects qualification; an image-query failure
is never reinterpreted as permission to accept an unidentified process.

The 1MiB per-stream limit is a sampling threshold and snapshot memory bound,
not a hard filesystem quota. A writer may overshoot it between samples.
The fixed output-limit control emits 1,100,000 bytes and then sleeps. Arbitrary
hostile writers, infinite process floods, injected threads, handle duplication
to unrelated processes, privileged escape, filesystem tampering, network files,
driver hangs and a wedged kernel are outside this supported scope. No Windows
API promises hard real-time return for arbitrary kernel or local storage stalls.
The controller never waits indefinitely for cooperative process completion;
it rejects overdue or unproved cleanup rather than asserting universal teardown
within five seconds. Ordinary local file operations and CloseHandle remain
synchronous, bounded in data/work count, with elapsed-budget failure reporting.

## CI qualification and evidence

The workflow keeps its narrow workflow-file push registration. Before the
expensive Linux setup/AOSP/CMake build, its controller matrix on windows-2022
and windows-2025 compiles only the native controller and controlled C# worker.
The actual target build remains fresh after this gate; no old artifact swap.
The real probe still runs on both Windows versions after verified production.

Seven controls exercise normal exit, exit 23, hang, parent exit with a sleeping
descendant holding the inherited files, controlled output excess, real denied
termination and unproved cleanup. The denied-termination case uses a duplicated
job handle lacking JOB_OBJECT_TERMINATE: access denial is a required failure,
then the full owner handle performs cleanup. It does not simulate an OS refusing
the full owner's termination. The unproved-cleanup case uses a duplicate lacking
JOB_OBJECT_QUERY after root reaping; it requires failure and no snapshot even
though the full owner attempted termination. It does not manufacture a leaked
hostile child. No failure-injection option is passed by the real probe.

Logs include controller/worker/script/workflow source blob IDs and SHA256s,
compiler and worker SHA256s, numerical root/job HANDLE values, PID/birth/image,
suspended thread count, pre-resume membership/census, raw root wait result,
exit code, total/active job accounting, termination request/result, separate
root/thread closure evidence, every owned/closed handle count, timing and errors.
The retained-child control requires a captured suspended child, one primary
thread, flushed writer readiness before root exit, explicit successful job
termination, retirement of every captured identity, and active==0.
TotalProcesses must equal the number of captured unique identities.
TotalTerminatedProcesses is
logged only as diagnostic data: it counts limit violations, not all manual kills.

Local checks performed: both PowerShell files parsed with the PowerShell AST
parser (zero errors), git diff --check, manual Win32 layout/control-flow review,
and exact original C++ blob checks. No local C#/C++ compilation, fixture run,
configure, push, CI dispatch, GitHub write, download or other-worktree mutation.
This checkpoint does not establish CI success; both cheap jobs and the actual
74-assertion target runs are required before qualification can be claimed.

Reviewed source blob manifest (Git's canonical filtered content):

| Path | Blob |
| --- | --- |
| `.github/workflows/tsf-format-component.yml` | `a9d9835431bd7132fa1d65ddbacd1ec88149010a` |
| `tools/windows4/run-tsf-component.ps1` | `4474bae13d5fe73038e8569be454753cdb21df43` |
| `tools/windows4/TsfJobSupervisor.cs` | `d1cf9319268df1bccf03177e7f8dde52a7f55510` |
| `tools/windows4/TsfJobWorker.cs` | `08f29f78f56195825d96de2f2f6632bf91133e1e` |
| `tools/windows4/test-tsf-job-supervisor.ps1` | `90f2ad84cc54f0dc1bbbc02c2ab8e75950cda5a8` |
| `compat/input/text_store.cc` (unchanged) | `f1068f05954c0173837fbc70a86eda681edab61d` |
| `probes/windows4/text_store_probe.cc` (unchanged) | `ea76cb9ccd00c50b143747e204c8aa2f4959e5a5` |

## API contracts consulted

- [Process handles remain valid until closed](https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers)
- [CreateProcess inheritance and explicit application](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)
- [Suspended, no-window and extended startup flags](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags)
- [Job membership, inheritance, termination and kill on close](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
- [Accounting fields and process-reference caveat](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_accounting_information)
- [Kernel job PID list layout and complete-list requirement](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_process_id_list)
- [Parent PID and process snapshot layout](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/ns-tlhelp32-processentry32w)
- [File sharing lasts until handle closure](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [Process signaling and resource retirement](https://learn.microsoft.com/en-us/windows/win32/procthread/terminating-a-process)
- [Microsoft console initialization source specification](https://github.com/microsoft/terminal/blob/main/doc/specs/%23492%20-%20Default%20Terminal/spec.md)

## Correction after run 40763475246

Both cheap controllers failed the descendant assertion at line 34; preflight
passed and the expensive compile/probes did not run. Windows-2025 logged root
5088 and descendant marker 6864, total=4/active=0, exit=0, successful termination
and readable output, but no RETAINED_WRITER_READY. Windows-2022 also logged
total=4/active=0 but its stdout reopen failed with a sharing violation. Normal,
exit23 and hang each logged total=2 on both runners. These logs do not identify
the extra processes or the remaining file reference. They do not prove writer
startup, and they never reached the output-cap case.

The accounting header defines TotalProcesses as cumulative associations, even
including a failed assignment on limit violation; it is not a two-PID identity
witness. Microsoft's console initialization specification describes starting
System32/conhost.exe from kernelbase for a console client without an existing
console, including clients created without a window. That is a source-supported
candidate cause for extra processes, not retrospective proof of these logs.

The correction admits no anonymous extra process. A helper must be observed
live in the actual job list with a retained handle, birth time, job membership,
exact system conhost image and actual owned root/descendant parent; at most one
helper is accepted per owned console client. Every other identity fails closed.
The root-start gate keeps controlled clients alive for this census. The worker
publishes its CREATE_SUSPENDED child's PID atomically and cannot resume it
until the controller captures its identity and one-thread census. The child
flushes RETAINED_WRITER_READY before publishing readiness; the root cannot exit
until the controller verifies the retained child's live handle and captures any
new console helper. No startup failure substitutes for this sequence.

The old sharing failure establishes that reopening with FileShare.Read did
not prove all write-access references had retired. Its owning PID is unknown.
The correction independently waits/signals/closes every captured process
reference, reconciles identities with cumulative accounting, observes job empty,
closes all controller file handles, then attempts one exclusive output open.
There is no sharing bypass or file-open retry. A remaining sharing violation
records its Win32 code and rejects the result. Future native logs must prove
this sequence; this checkpoint does not claim the historical race is reproduced
or causally closed by a passing execution.

The output negative requires started control, stdout bytes >1MiB, exact
output-limit reason, exit=124, successful termination, zero captured bytes and
no unrelated controller errors. Exit-time size is checked as well as sampled
size. Timeout and denied-query controls require actual started hang evidence.

Win32 PID enumeration cannot recover a live image/retained handle after an
unobserved process has exited. The real C++ probe has no added startup barrier:
if its console helper exits before capture, or a helper has an unexpected
parent/image, the controller rejects it with precise identity/census errors.
This bounded fail-closed fallback is intentional. Qualification still requires
both cheap native matrices followed by fresh actual 74-assertion runs; source
inspection cannot certify either platform or eliminate the documented capture
race for an uncooperative fast native client.

## Peer review questions

1. Do the x64 STARTUPINFOEX, extended job limit, accounting and thread-entry
   layouts and access masks match both intended Windows runners?
2. Does releasing the signaled root/thread before the job-empty observation,
   with no external admission or escaped writers in scope, adequately establish
   the intended descendant completion witness? Do both matrix logs confirm it?
3. Do fault controls demonstrate real denied rights and conservative failure
   without making a claim about full-owner termination denial or kernel stalls?
4. Is the controlled-process/local-storage scope and sampled storage threshold
   appropriate, or would a future requirement need a separate quota-backed volume?
5. Do both cheap matrix jobs emit complete native evidence before the fresh
   expensive build, and do subsequent actual runs retain exactly 74 assertions?

The separate services controller was not edited. Reuse there should follow
qualification of this protocol, not an assumption of shared proof.
