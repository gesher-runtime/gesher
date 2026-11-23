# Child waits driven by lifecycle notifications

This is the native child-wait efficiency leaf for [#56](https://github.com/gesher-runtime/gesher/issues/56)
and [#178 NATIVE-12](https://github.com/gesher-runtime/gesher/issues/178).
The baseline is published main `df3aff782e2d411aad2d07ca39f0b21ee2ddbfe2`.
The complete samples, compiler, platform and source hashes are in
[the measurement JSON](child-wait-notifications-2026-11-23.json).

## Change and scope

The existing Win32 registered wait already knows when a spawned child exits.
Before this change, `CollectOne` nevertheless scanned the child registry and
called `InterruptibleSleep(20)` repeatedly until it observed that result. With
the ordinary default SIGCHLD disposition, signal dispatch supplied no wakeup.

Successfully registered children now use an untimed condition wait. Child
registration, actual exit and removal publish a generation change. The waiter
captures the generation together with its eligibility scan, then checks it under
the same signal mutex that orders condition arming and notification. An exit or
another waiter's reap before arming causes an immediate rescan; a change during
arming cannot pass the signal mutex until the waiter has released it to sleep.
Signal delivery still uses the existing production helper and pending-state
machinery. No additional polling worker or guest-facing capability is introduced.

Removal notifies competing waiters, including a waiter that missed the earlier
exit notification and now needs ECHILD. Watch retirement and process-handle close
run after releasing the child registry mutex. Once the exit callback publishes
the child state, its remaining work does not access the child record or process
handle, so its tail can overlap reaping and a later PID incarnation.

One explicit exceptional path remains timed: if
`RegisterWaitForSingleObject` fails, an already spawned child still needs an
owner. Such an unregistered child uses a zero-time native process-state probe
with the existing 20 ms sleep between observations. This path reads the actual
exit code, retains ownership on observation errors and propagates those errors.
Registration failure also wakes an already blocked any-child waiter, allowing
it to select this fallback. There is no claim that this exceptional path is
free of polling. The old path did not check registration failure and could wait
forever without observing that child's native exit.

The existing per-child Windows registered waits and OS pool threads remain.
[Microsoft's registration contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-registerwaitforsingleobject)
describes the infinite timeout, one-shot flag and pool callback execution;
the implementation preserves all three. The nonblocking unregister operation
can return while a callback is active, as documented by
[UnregisterWaitEx](https://learn.microsoft.com/en-us/windows/win32/sync/unregisterwaitex).
This change does not claim to remove those pool threads or provide a
whole-process handle/thread census.

## Android-visible behavior and outstanding gaps

[Android 16 r4 Bionic wait.cpp](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/wait.cpp)
delegates `wait` and `waitpid` to `wait4`, and `waitid` to its kernel entry.
The retrieved source SHA256 is
`af86e67822396ad400736883dac3c9adc33d98f562c02cd9f9235b239ae79133`.
[Android common kernel's wait implementation](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2026-03_r42/kernel/exit.c)
registers a child wait queue before checking eligibility and uses interruptible
sleep, with WNOHANG and ECHILD handled by the eligibility scan. These are semantic
references; no kernel implementation is copied into Gesher.

This leaf preserves the existing positive-PID and any-child exit selection,
WNOHANG return/status behavior, exit-status conversion and exactly-once reaping.
The new public corpus observes an actually running child before testing
WNOHANG, then observes real child identities and status through concurrent and
fast-exit cases. It does not replace process inputs or fabricate successful exit.

The following existing differences remain separate compatibility work:

| Area | Existing boundary retained by this leaf |
| --- | --- |
| EINTR and SA_RESTART | `CollectOne` already restarts after a delivered handler regardless of SA_RESTART. It propagates signal registration/delivery errors. The new test explicitly preserves that behavior rather than claiming Android EINTR parity. |
| SIGCHLD ignored/no-zombie policy | Explicit SIG_IGN and SA_NOCLDWAIT do not currently cause automatic child-record retirement. This optimization does not add that policy. |
| Stopped/continued children | The child registry records exits only. The public live-child test includes WUNTRACED/WCONTINUED without generating either event; that is not evidence of stop/continue support. |
| Process groups and options | The existing nonpositive-PID selection and incomplete option validation are unchanged. |
| waitid and wait4 rusage | No new waitid implementation or resource-usage accounting is supplied. |
| SIGCHLD information and fork | Existing external SIGCHLD information and fork/exec limitations are unchanged. This is not a fork implementation or full process-parity claim. |

## Reproducible measurement

The same harness compiles the complete production child registry, collector,
public wait wrappers and signal-wait helper from either the baseline or working
tree. Win32 handles/registration, guest-thread registration and signal dispatch
are declared host boundaries. The condition variable and competing host threads
are real. Default SIGCHLD in this measurement supplies no artificial wakeup,
matching the relevant production branch.

Runs alternate baseline, candidate, candidate, baseline. Each run has five
samples for one waiter and five for eight waiters. All children are registered
before starting the waits. Each sample waits 207, 210, 213, 216 or 219 ms before
delivering real fixture completion state through the production callback. The
different idle periods avoid an accidental exact 20 ms boundary. Every child
identity and exit status is checked. Native probes remain zero for the registered
path in both versions.

Medians over ten samples per version and waiter count:

| Registered waiters | Metric | Baseline | Candidate |
| ---: | --- | ---: | ---: |
| 1 | Wait-helper entries before exit | 11 | 1 |
| 1 | Timed condition calls before exit | 11 | 0 |
| 1 | Untimed condition calls before exit | 0 | 1 |
| 1 | Waiter thread CPU for the complete wait | 296.047 µs | 38.823 µs |
| 1 | Callback delivery to all results collected | 8.146 ms | 0.119 ms |
| 8 | Wait-helper entries before exit | 88 | 8 |
| 8 | Timed condition calls before exit | 88 | 0 |
| 8 | Untimed condition calls before exit | 0 | 8 |
| 8 | Aggregate waiter thread CPU for the complete waits | 1,693.547 µs | 435.820 µs |
| 8 | Callback delivery to all results collected | 8.027 ms | 0.551 ms |

The principal result is removal of periodic timer-driven child scans in the
successfully registered path. CPU includes the waiter entry, its sleep and final
collection, measured with CLOCK_THREAD_CPUTIME_ID; it excludes callback execution
on the controlling thread. Latency includes the controlled completion callback
and collection of all futures. These Linux component timings do not measure
Windows process exit, pool dispatch, ART, application startup, whole-process CPU
or RSS. Scheduler variation is visible in the raw samples. Child scans remain
linear in the number of eligible records when a real event or signal occurs;
this change does not introduce a child-selection index.

Reproduce a candidate and unchanged-source measurement with:

```sh
python3 tools/test_child_wait_notifications.py --benchmark
python3 tools/test_child_wait_notifications.py --benchmark \
  --source-ref df3aff782e2d411aad2d07ca39f0b21ee2ddbfe2
```

## Contract evidence and validation

`tools/test_child_wait_notifications.py --sanitize` executes 68 host checks and
five real Linux public process/thread checks. The host cases include exit before
arming, another waiter reaping before arming, exit between predicate and sleep,
specific-PID eligibility under unrelated exits, signal delivery during a wait,
registration failure, native wait/status-query errors, a newly unregistered
child while any-child wait is already armed, callback tail/PID reuse, signal
registration error, and eight waiters collecting 48 children exactly once.

The unchanged baseline fails six relevant assertions in the same harness:
retirement under the registry lock, both pre-arm cases, predicate-to-sleep wait
kind, signal/selection polling, and concurrent waiter polling. Its unsupported
watch-registration-failure case is explicitly omitted because it cannot observe
the child's exit; the new implementation must execute and pass that case. The
shared real Linux public corpus passes against the reference kernel.

The public corpus is included in the existing native exec2 driver and its exec3
JNI/app reuse. Windows controllers retain the original ten assertions and require
the new checks, raising the expected successful total to 15. Full fork CI must
build and execute those actual Windows/ART routes; they were not run in the Linux
workspace. Both complete changed production translation units were crosscompiled
with the CI-pinned LLVM-mingw 20261006 toolchain for x86-64 and ARM64 Windows.

Other relevant final production-source replays passed:

| Gate | Result |
| --- | --- |
| New contracts with fortified libc headers, ASan and UBSan | 68 host + 5 real Linux checks |
| Existing sleep contracts | 107 production-component + 30 Linux checks |
| Existing boottime/resume contracts | 35 assertions |
| Existing signal mask/action/delivery contracts | 46 original + 470 production-helper checks |
| Source/DAG policy with transition negative controls | 33 mutations, 4 repinned property mutations and 5 repinned child-wait mutations rejected |
| Selected supplier metadata controls | 36 semantic mutants and authority-hash negative control passed; no runtime claim |

Local commands used ASAN_OPTIONS=detect_leaks=0 because LeakSanitizer cannot run
under this workspace's ptrace environment. AddressSanitizer and UBSan remained
enabled. The committed CI invocation does not disable LeakSanitizer.

```sh
python3 tools/test_child_wait_notifications.py --sanitize
python3 tools/test_sleep_contracts.py --sanitize
python3 tools/test_boottime_resume.py --sanitize
python3 tools/test_bionic_signal_masks.py --sanitize
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
python3 tools/net-tcp-priority/test-selected-supplier.py
```

## Source qualification and CI recipe

The qualification adds an exact reversible transition for the three complete
production files. It checks current whole-file hashes, reverses the pinned patch,
compares every byte with published `df3aff78`, then reconstructs the current
files before running all previous OFD checks. The original OFD manifest, all 71
source digests, its patch and runtime approval remain unchanged. The existing
property and memmem normalizations still run. Updating candidate hashes alone
cannot approve behavior outside this exact transition; negative controls verify
that rejection. The new receipt explicitly has native_runtime_approval=false.

As in the other issue-56 leaves, the CI reusable framework reference changes from
the unpublished `06d175a84affff8914c00151269b0aa2f7f4032a` to published
`6afcb6e671fc4f2d08a4a98220f9d9a906a327bf`. Both refer to the same framework recipe
blob `834692396e307a6cd9251c527b7a91ab402d13ac`; runner routing, source identity,
permissions and all runtime gates are unchanged. This prevents the inherited
workflow-not-found startup failure and is not a reduction of CI coverage.
