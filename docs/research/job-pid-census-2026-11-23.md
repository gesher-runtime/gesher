# Observe the Windows job PID census across retained-root exit

**Status: diagnostic only. The original #360 failure remains unresolved.**

The genuine full manual [run 45194542579, Windows-2022 controlled worker job 137838581059](https://github.com/yom-chadash/gesher/actions/runs/45194542579/job/137838581059) failed at published #360 head `07ab5dd901e4e380f96ddb3865945afc29476d5f`. The WinRT provider exited with code zero. Its final receipt records one cumulative job association, the retained root PID 780, an empty job, and seven of seven handles closed. Before output could be read, the controller rejected a PID-list observation with `Incomplete job PID census`.

That guard covers a negative or oversized returned count and unequal assigned/returned counts. The original log did not preserve those counts or the returned IDs. Final retirement alone cannot establish which observation occurred, and it does not authorize suppressing the error. The complete original decoded log and its receipt are retained in the [JSON record](job-pid-census-2026-11-23.json).

The failed controller and geocoder entry are byte-identical to public main `e505ad227523aaca566d05b8af6e466495c819cc`. PR #393's separate metadata-file-object change does not modify PID enumeration. This diagnostic leaves `TsfJobSupervisor.cs`, its qualified copy, its original validation, every provider assertion, output cap and existing deadline unchanged.

## Actual Windows observation

An isolated helper runs before the ordinary geocoder compiler/provider. In each of 16 lifecycles it creates a real detached `cmd.exe /d /c exit 0` with its initial thread suspended, assigns it to a new kill-on-close job limited to one active process, verifies the actual membership, creation time, initial accounting and exact suspended PID, and only then resumes it. It retains the process and thread handles throughout exit observation.

The helper records the actual [`JOBOBJECT_BASIC_PROCESS_ID_LIST`](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_process_id_list) assigned count, returned count and valid IDs while the root is suspended, during its exit if an incomplete raw observation occurs, after the retained root signals, and after its handles close. It also invokes the actual controller's private `Pids` method through reflection. An exact `IOException` with the existing incomplete-census message is counted as a diagnostic observation; other errors still fail. Raw snapshots and controller invocations are separate kernel queries, so they must not be presented as one atomic observation.

Every probe lifecycle has one shared 5,000 ms budget, including retirement and cleanup. No child receives inherited handles. Cleanup acts on retained handles and the owned job; it never finds or kills by PID. Unproved retirement and failed handle closure remain failures, with those handles quarantined for the CI process lifetime. The final proof requires the exact single cumulative association, empty job, signaled root and all three owned handles closed.

The probe may observe zero mismatches. It retains that result and does not retry until a mismatch appears. The ordinary WinRT provider and all of its original assertions still execute afterward; a genuine provider/controller failure remains a failure. The detached command is an explicit diagnostic input, never a substitute for the actual provider, and no capability or runtime behavior changes.

## Available validation and limits

The complete unchanged controller and new helper compile with PowerShell's actual C# compiler locally, and both PowerShell entry files parse. Thirteen host checks cover non-CI rejection, null/wrong controller rejection, invocation counting and preserving unexpected exception types/messages. Run `pwsh -NoProfile -File tools/test_pid_census_exit_probe.ps1` to reproduce those checks. The isolated reflection controls do not execute Windows APIs. The existing selected-source policy passes without a normalization exception because the controller and protected sources are unchanged.

Actual Windows PID-list observations and full manual CI are still pending. This commit is not an efficiency improvement, a runtime repair, a timeout relaxation, or evidence that the archived failure was external. A repair requires the newly observed kernel evidence and a separate scope/lifetime review.
