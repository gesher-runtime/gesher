# TSF owned job/file supervisor: source checkpoint

This is a source-only replacement of the controller at base
`da559831bc330edae6d2432336588f5a58febf80`. It has not been compiled or run.
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
original handle. Root/thread handles are closed before requiring job accounting
ActiveProcesses==0, so our references cannot impede that accounting transition.
The job remains queryable until that observation. Closing the job alone is
never accepted as successful cleanup. Errors or expiration make Success false.

Only a signaled root, observed empty job, and successful closure of every
owned handle permit snapshots. Each regular file is length-checked before
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
The implementation does not identify, reopen, or kill a process by PID or
guess an executable path for termination.

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
The retained-child control requires total processes==2, explicit successful job
termination and active==0, not just script PASS. TotalTerminatedProcesses is
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
| `tools/windows4/TsfJobSupervisor.cs` | `ae7db3527939a2a5cc88046ff94f411e5433460b` |
| `tools/windows4/TsfJobWorker.cs` | `fd2b7768cc227cf2352331405bdaa1050f257070` |
| `tools/windows4/test-tsf-job-supervisor.ps1` | `59c054aeb7ff75749f4a8e2a60c30193f23d629e` |
| `compat/input/text_store.cc` (unchanged) | `f1068f05954c0173837fbc70a86eda681edab61d` |
| `probes/windows4/text_store_probe.cc` (unchanged) | `ea76cb9ccd00c50b143747e204c8aa2f4959e5a5` |

## API contracts consulted

- [Process handles remain valid until closed](https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers)
- [CreateProcess inheritance and explicit application](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)
- [Suspended, no-window and extended startup flags](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags)
- [Job membership, inheritance, termination and kill on close](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
- [Accounting fields and process-reference caveat](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_accounting_information)

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
