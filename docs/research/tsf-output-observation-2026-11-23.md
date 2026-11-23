# Keep TSF output observation independent of a pending writer

This is a common CI-controller ownership repair discovered while monitoring #298. It changes no Android runtime operation, acceptance threshold, process identity rule or workflow deadline. Actual Windows qualification of the new control remains pending; the archived timing failure is not attributed conclusively to one kernel operation.

## Observed failure and its limits

The original [Windows 2022 controller job](https://github.com/gesher-runtime/gesher/actions/runs/45178885879/job/137715173763), attempt 2, tested merge `14e9d34b8e515fd2ee0de761c04d907b9ff8e06e` of #298 head `ec2ed793952d0060df0e1e0c5485b9808c527f69`. Its output-limit control failed the unchanged 5,000 ms lifetime assertion. The paired [Windows 2025 job](https://github.com/gesher-runtime/gesher/actions/runs/45178885879/job/137715173749) passed the same source and control:

| Actual output-control receipt | Windows 2022 | Windows 2025 |
| --- | ---: | ---: |
| Requested main budget | 3,500 ms | 3,500 ms |
| Recorded main duration | 6,666 ms | 88 ms |
| Recorded cleanup duration | 18 ms | 13 ms |
| Real stdout length | 1,552 bytes | 1,100,016 bytes |
| Termination reason | `timeout` | `output-limit` |
| Owned/closed handles | 8 / 8 | 8 / 8 |
| Root signaled / job empty | true / true | true / true |

The failed run had not reached the 1 MiB cap. Its cleanup was proved, but that does not excuse its excessive main duration or satisfy the output-limit assertion. The log contains no operation-level timing or scheduler trace. A blocked synchronous call, descheduling and filesystem delays cannot be distinguished from this receipt alone. The separate #298 DNS deadline failure is outside this change.

All six original controller-source receipts match the #298 branch bytes. The old controller is also byte-identical on main `bcfb5044029d486e17138dc417f3ced81524cf3f` and its successor `3b2eaca95b5ddd9cee3c3db4e430460e512eb355`, SHA-256 `070a03b1e1cc55538cd8bd5e5ff8bb2c65db5998b4d5442fc084f97f8c875359`. The latter main merged #298 while this repair was being prepared; the repair targets public main and does not update that closed feature branch. Both complete original logs are retained in the accompanying evidence archive.

## Documented ownership hazard and repair

The controller creates synchronous regular stdout/stderr files and passes their handles to the suspended child through the existing explicit inheritance list. It previously used those same file objects for live `GetFileSizeEx` polling. [Microsoft's file-handle documentation](https://learn.microsoft.com/en-us/windows/win32/fileio/file-handles) explains that inheritance and duplication share a file object. [Microsoft's synchronization explanation](https://devblogs.microsoft.com/oldnewthing/20140711-00/?p=523) explicitly warns that a synchronous operation can prevent another handle user from accessing that object; duplicating the handle does not remove the serialization.

Before launching the child, the controller now obtains one [ReOpenFile](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile) handle for each output file. Each observer requests only `FILE_READ_ATTRIBUTES`, retains compatible read/write sharing, and refers to the same underlying file through its existing handle. This avoids reopening a replaceable path and creates an independent file object for observation. [GetFileSizeEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfilesizeex) documents that metadata access as sufficient.

The observers are immediately entered into the existing owned-handle list, are absent from the child's inheritance list, and are used for both live and final size observations. This also avoids returning to the writer's object for a final size check after a failed retirement attempt. Failure to open an observer follows the original controller-error and cleanup path before child launch; there is no fallback to shared-object polling or synthetic size result.

The inherited writer handles, byte cap, child barriers, absolute main deadline, job membership and cumulative census checks, termination, wait and close requirements remain unchanged. All observers close through the existing cleanup before the exclusive output snapshot. The cost is two native handles per controller invocation, with no new controller thread, pipe or background reader.

Independent file objects remove this particular writer-object serialization edge. They do not make arbitrary filesystem operations preemptible or establish a universal wall-clock bound under all scheduling or storage conditions.

## Required actual Windows controls

The existing required Windows controller job now compiles and calls `TsfOutputMetadataChecks` before all its original controls. The new helper uses an actual regular file, real Win32 handles, a held byte-range lock and a synchronous [LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex) request. It observes the blocked writer using [GetThreadIOPendingFlag](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadiopendingflag).

The old-object comparison uses `DUPLICATE_SAME_ACCESS` so it preserves the original writer's access rights. A reader is armed before querying size, and its noncompletion is observed for 250 ms while the writer remains pending. That is a bounded observation; the readiness event alone does not prove that this reader entered its kernel call.

The stronger positive check calls the exact controller observer helper and requires the real file size to be returned while the writer still has actual pending I/O and its independent lock holder has not released the range. The new observer must be noninheritable and reject a write with `ERROR_ACCESS_DENIED`. Invalid writer input must fail with `ERROR_INVALID_HANDLE`.

After releasing the holder, both native-call threads must join within one shared cleanup budget. All handles and events retire before the exclusive file snapshot and deletion. If retirement or handle closure is unproved, the fixture retains the relevant owners rather than disposing resources still in use. It logs the original/cleanup exception before rethrowing with its stack intact, so a later outer directory-cleanup error cannot erase that evidence. No held file is blindly deleted.

These are committed Windows controls, **not locally executed Windows results**. The full original output-limit case still requires actual bytes beyond 1 MiB, exact `output-limit` classification, no captured output, proved termination/retirement and the original duration ceilings. The fixture does not retry that product assertion.

## Local validation and reproducibility

Both complete C# types compile using PowerShell 7.5.4, and the actual PowerShell entry parses. Its CI-only public guard rejects the local host. A separate host control executes the actual fixture's native-library resolution failure and verifies that its original diagnostic remains visible when a modeled later cleanup error replaces the thrown exception. A compiled mutation deleting only that diagnostic fails the specific preservation assertion. This checks error reporting, not Win32 locking.

The local PowerShell resolver raises `ArgumentNullException` while resolving an unavailable native library from its in-memory assembly. Earlier host-harness expectations about exception type/stack are retained as rejected harness runs. The final control checks the actual native-resolution boundary and exception evidence rather than claiming Windows execution.

Source reconstruction removes only the new source receipt, compilation input and helper invocation from the PowerShell entry and recovers every original byte. Similarly, removing the exact observer additions and replacing the four size-query handle arguments recovers the old controller. The worker, complete `ci.yml`, component workflow, 1,500/3,500/45,000 ms main budgets, 5,000 ms assertions and cleanup budget are unchanged.

The [JSON index](tsf-output-observation-2026-11-23.json) and [evidence archive](data/tsf-output-observation-2026-11-23.tar.gz) retain original failure/success logs, exact source receipts, compilation/error-reporting controls, the rejected local harness runs and source-preservation proof. The archive's `test-host.ps1 -SourceRoot <checkout>` reproduces the Linux compilation/diagnostic boundary only. Actual pending-I/O behavior and full fork acceptance require a genuine manual `ci.yml` dispatch on the published head; no green result is claimed here.
