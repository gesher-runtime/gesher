# N53: Android uptime and elapsed-realtime domains

This change addresses the clock-domain part of issue #56. It does not claim
that the whole issue, wakeup alarms, or physical suspend qualification is complete.

## Contract and implementation

Android distinguishes [uptime and elapsed realtime](https://developer.android.com/reference/android/os/SystemClock):
uptime excludes deep sleep, whereas elapsed realtime includes it. A common
QPC clock for both cannot represent those contracts across Windows suspend.

The shared implementation in `compat/posix/include/gesher/clocks.h` uses
[QueryUnbiasedInterruptTimePrecise](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryunbiasedinterrupttimeprecise)
for awake time and
[QueryInterruptTimePrecise](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryinterrupttimeprecise)
for elapsed time including sleep/hibernate. Both report 100 ns units; conversion
to signed nanoseconds saturates instead of overflowing. Windows 10 is already
the repository's minimum target. Their imports use `mincore`, a public dependency
of `gesher_posix`, after the ordinary Win32 libraries including `kernel32`.
LLVM-MinGW does not ship the standalone realtime API-set import library; putting
the umbrella `mincore` first would intercept unrelated Win32 imports. Suspend/resume
registration uses the separate `powrprof` Windows system library. Host PE import
policies admit only `api-ms-win-core-realtime-l1-1-1.dll` and `powrprof.dll` as
new system DLLs for this integration; translator
roles and unknown/private DLL rejection remain unchanged.

| Consumer | Selected domain |
| --- | --- |
| Guest MONOTONIC, MONOTONIC_RAW and MONOTONIC_COARSE | Unbiased interrupt time; RAW/COARSE retain their existing alias behavior |
| Guest BOOTTIME | Biased interrupt time |
| ART MilliTime, MicroTime, NanoTime; libutils uptime; System.nanoTime | The same awake domain |
| libutils SystemClock.elapsedRealtime[Nano] | The same BOOTTIME domain, including its Windows-specific dispatch |
| Guest pthread/semaphore/futex monotonic deadlines | The same awake epoch as their Android clock reader |
| Relative REALTIME sleeps and timerfds | Awake duration, unaffected by wall-clock changes |
| BOOTTIME sleeps and timerfds | Elapsed duration including sleep |
| Absolute REALTIME sleeps and timerfds | Wall time |
| Android audio timestamps, synthetic/file capture pacing, file/null sink pacing | Awake domain |
| MediaCodec rendered-frame system-nano and missing input timestamps; video-only MediaPlayer pacing | The same awake domain as Java System.nanoTime and audio pacing |
| Host winpthreads clock_gettime and host pthread condition variables | Unchanged host QPC epoch |

`AndroidClockGettime` is an explicit adapter, not a global override of the host
clock. ART's existing GESHER patch enables `ART_USE_FUTEXES`; ART absolute
monotonic dates therefore use the same adapter as Gesher's futex backend.
Replacing the host pthread clock globally would instead break its own absolute
wait implementation. Wall/CPU clocks continue to use their existing backend.
Architectural counters in ARM translation and internal protocol timestamps are
not assigned Android uptime semantics by this change.

### Remaining relative timeout work

This change does not convert every use of `steady_clock` in the runtime.
`signal_bionic.cc::Sigtimedwait`, the native `sigtimedwait`, and socket
`Recvmmsg`/`PollTimeout` still use host steady-clock relative accounting. Their
existing suspend behavior needs a separate timeout-transport change and tests;
they do not receive the newly changed Android absolute clock values. The
ordinary signal sleep transport retains host condition-variable deadlines.
The outer nanosleep loop checks its remaining Android duration after each
transport return. BOOTTIME clock_nanosleep additionally uses the resume-aware
predicate and notification described below. No Android absolute date is passed
to a host condition variable.

Internal prepare/performance tracing, probe deadlines and ARM's emulated
architectural counter are intentionally left on their host clocks. Device and
guest architectural-counter qualification is outside this Android API clock
patch.

`ClockNanosleep` observes interruption time in the selected domain before
running a signal handler, so an interrupted relative BOOTTIME sleep reports
the remaining BOOTTIME duration, not uptime duration.

## Resume-triggered delivery correction

Windows [relative threadpool timers exclude time spent asleep](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-setthreadpooltimer).
The [maintainer's review of PR #62](https://github.com/gesher-runtime/gesher/pull/62#pullrequestreview-5929639083)
correctly identified a transport bug in the initial one-second slicing scheme:
an expired BOOTTIME deadline could retain an extra awake-time budget after
resume, leaving poll/epoll not ready while direct read could return a count.
The original test manually fired an ineligible native callback and missed this
disagreement. The correction starts from maintainer commit
`35c55f3b7024fd5f0b7cd91f2b46b77f87aadd26` and preserves its five futex-test fixes.

`resume.cc` registers a callback with
[PowerRegisterSuspendResumeNotification](https://learn.microsoft.com/en-us/windows/win32/api/powerbase/nf-powerbase-powerregistersuspendresumenotification),
available before the repository's Windows 10 minimum. It handles both
`PBT_APMRESUMEAUTOMATIC` and `PBT_APMRESUMESUSPEND`, including duplicate events.
BOOTTIME timerfds subscribe before publication. A resume callback cancels the
old native arm, accounts for expired periods, signals existing read/poll/epoll
readiness, and rearms any future deadline using its remaining BOOTTIME duration.
The one-second BOOTTIME cap is removed. Wall-clock adjustments do not change
this transport or its deadline.

BOOTTIME sleeps subscribe before entering the signal wait. Their deadline
predicate and resume notification use the same signal-state mutex; a resume
between the clock sample and condition-wait handoff cannot be lost. A resume
before subscription is caught by the subsequent clock predicate. A future
deadline waits for its actual remaining duration, while a real signal retains
EINTR and the pre-handler BOOTTIME remainder. Notification failure is explicit
(ENOMEM or EIO); MONOTONIC and REALTIME paths do not require registration.

### Lifetime and lock ordering

The callback context has process lifetime. A runtime image that links the
dispatcher registers once; because `gesher_posix` is a static library that can
be linked into a host DLL, the callback's module is pinned with
[GetModuleHandleExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexw)
using `FROM_ADDRESS | PIN`. This deliberately keeps that host runtime image
loaded until process exit. Per-consumer subscriptions remain removable.

[PowerUnregisterSuspendResumeNotification](https://learn.microsoft.com/en-us/windows/win32/api/powerbase/nf-powerbase-powerunregistersuspendresumenotification)
does not document callback quiescence. Consumer teardown therefore uses its
own lock: the dispatcher snapshots shared observer states under its registry
mutex, releases that mutex, then locks each observer before calling it. Reset
joins that observer, disables it, and removes it from the registry. A previously
captured snapshot remains safe and cannot call a retired context. No registry
lock is held while acquiring a consumer lock or joining its callback.

Timer destruction resets the resume subscription before taking timer locks or
joining native timer callbacks. The existing settime/disarm `changing` state
prevents resume publication during a reprogram transaction. An admitted read
retains its shared timer owner through descriptor close. Sleep subscription
destruction occurs after its inner wait function releases the signal mutex,
including signal and expired-deadline returns.

LLVM-MinGW's pinned header declares the power functions but omits the callback
payload and flag. A guarded local declaration matches the documented callback
signature and Microsoft's
[powrprof.h ABI](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/powrprof.h).
The new import library dependency is `powrprof`.

Windows still schedules and delivers the power callback; no immediate-return
or wake-from-sleep guarantee is claimed. The corrected path does not require
the old relative timer's remaining awake budget to elapse after that callback.
AAudio BOOTTIME timestamps remain unsupported by the existing API adapter.

## Evidence recorded on Linux

The new runner compiles exact production bodies and the actual patches applied
to pinned AOSP source; see [fixture provenance](../../tests/clock-domains/README.md).
It drives separate awake, suspend, wall and QPC epochs and checks domain
agreement, wall jumps, sleep interruption/remainders, absolute deadlines,
conversion overflow, and audio sample production through suspend/resume.

| Command (`python3 tools/…`) | Result |
| --- | --- |
| `test_clock_domains.py --sanitize` | 71 assertions pass |
| Same command with `--source-ref 6f5e23a598c10906e4518045bc8d73f5c0cf3a9f` | Compiles; 30 semantic assertion failures, expected negative control |
| `test_timerfd_contracts.py --sanitize` | 133 host assertions and 68 Linux reference assertions pass |
| Timerfd `--case suspend --skip-linux-reference --sanitize --source-ref 35c55f3b7024fd5f0b7cd91f2b46b77f87aadd26` | Compiles; 11 of 31 assertions fail, expected negative control |
| Same timerfd control at `b136d57afbb368661d99ef097a1d8651b0edebaf` | Compiles; only fresh poll before callback dispatch fails (one of 31) |
| `test_boottime_resume.py --sanitize` | 35 assertions across six groups pass |
| BOOTTIME sleep `--case resume --case partial --case registration-race --sanitize --source-ref 35c55f3b7024fd5f0b7cd91f2b46b77f87aadd26` | Compiles; seven of 18 assertions fail, expected negative control |
| `test_sleep_contracts.py --sanitize` | 107 host checks and 30 Linux reference checks pass |
| `test_futex_contracts.py --sanitize` | Maintainer-corrected 81 public, 166 guest and 152 concurrency assertions pass |
| `test_ordinary_os_contracts.py --sanitize` | All host groups and Linux reference pass |

The timer fixture now tracks native `awake_due` separately from BOOTTIME and
never fires an ineligible native timer in its suspend group. It covers blocked
read and readiness-event waits, retained reads after final descriptor close,
partial suspension, duplicate notifications, UTC changes, registration failure,
stale snapshots, descriptor reuse, disarm and final-close callback quiescence.
The fresh-poll case also advances BOOTTIME before dispatching the power
notification: readiness lookup samples the selected clock under the timer
mutex, matching the expiry already observed by read/gettime. The object
registry lock is released before acquiring that mutex, and its shared timer
reference protects the refresh across a concurrent descriptor close. A later
resume callback cannot count the poll-refreshed expiration again.
The sleep fixture compiles the actual dispatcher, signal wait and public sleep
wrapper together and controls the relative native wait eligibility. Existing
clock/ordinary-sleep fixtures remain arithmetic tests rather than evidence for
resume delivery.

The development sandbox is traced, which prevents LeakSanitizer from running.
These local executions used `ASAN_OPTIONS=detect_leaks=0`; AddressSanitizer and
UndefinedBehaviorSanitizer remained enabled. CI does not disable leak checks.

The repository-pinned LLVM-MinGW compiler also compiled and linked the real
Windows clock probe against Windows headers and import libraries:

```sh
x86_64-w64-mingw32-clang++ -std=c++20 -Wall -Wextra -Werror \
  -D_WIN32_WINNT=0x0A00 -I compat/posix/include \
  tests/native/clock_domains_windows_test.cc -pthread -lmincore \
  -o clock_domains_windows_test.exe
```

This is compile/link evidence, not an execution result.

The review correction also cross-compiles the actual `resume.cc`, `timerfd.cc`,
`signal_bionic.cc`, `extra_sys_bionic.cc` and new Windows acceptance probe with
the pinned LLVM-MinGW headers and the repository's 64-bit file-offset flags.
The resume dispatcher is separately linked against the real `powrprof` import
library. Full Windows runtime integration remains a CI gate.
The ARM64 workflow also builds `boottime_resume_windows_test.exe` and verifies
its PE architecture; that build is not an ARM64 execution result.

## Windows and Android acceptance gates

The Windows time track now runs `clock_domains_windows_test.exe`, which
brackets the actual readers independently and verifies that the host pthread
clock still uses QPC. The existing `native3_cpu_clock_test.exe` brackets actual
patched ART clocks using unbiased interrupt time. The dex-native fixture also
brackets `System.nanoTime` with `SystemClock.uptimeNanos`. These Windows/ART/JVM
executions require CI or a configured Windows build; they were not run in the
Linux development environment.

`boottime_resume_windows_test.exe` is now built and run by the time track. Its
default mode exercises real power registration, read/poll/epoll APIs, a public
guest BOOTTIME sleep and expiration counts while Windows stays awake. Its
20 ms deadlines do not include a waiter-entry barrier, so this mode does not
prove that every waiter entered its blocking operation before expiry. The
controlled host regressions separately enforce admitted waiters. It prints
`physical=no`; this mode cannot qualify physical resume.

For physical evidence, run:

```powershell
.\out\clock_domains_windows_test.exe --after-suspend
.\out\boottime_resume_windows_test.exe --after-suspend
```

Suspend or hibernate the machine for at least five seconds, resume, then press
Enter. Record the Windows build, machine/power mode, suspend type and printed
excluded nanoseconds. Repeat separately for suspend and hibernate where the
machine supports them. The probe requires BOOTTIME to advance at least one
second more than uptime, without assuming a zero clock origin (checked Windows
builds can deliberately bias interrupt clocks).

For the BOOTTIME transport probe, follow its separate instructions: enter sleep
within 20 seconds, stay suspended for at least 75 seconds, then resume. It arms
60-second absolute BOOTTIME and MONOTONIC deadlines, waits on actual read/poll/
epoll and the public guest sleep, and records both elapsed domains at the power
notification. A run qualifies only if the BOOTTIME deadline elapsed while the
MONOTONIC deadline remains future. The probe checks readiness after its timer
resume observers run, blocked-wait completion and periodic counts. Fresh poll
can also account for an expired deadline, so that readiness sample alone is
not evidence that an already-blocked waiter was notified. The admitted-wait
host regressions check that transport separately. Record its complete
output separately for suspend and hibernate. No physical run was performed in
the Linux implementation environment.

A full N53 acceptance run still needs real Windows suspend/resume while guest
MONOTONIC and BOOTTIME sleeps/timerfds are outstanding, verifying the remaining
awake timeout versus expired BOOTTIME and periodic expiration counts, together
with a pinned Android oracle. Device audio timestamps and media behavior across
physical resume also remain runtime qualification gates. Simulated suspension
proves routing/arithmetic and regression sensitivity; it cannot establish
Windows power-management or device behavior.

## Java System.nanoTime production-domain correction

Windows CI at e2648526 found that libcore's System_nanoTime C entrypoint still used the host winpthreads CLOCK_MONOTONIC/QPC epoch, even after ART's separate NanoTime helper had been corrected. The strict enclosed SystemClock.uptimeNanos assertion exposed this production mismatch. A shared C-compatible awake_clock.h now supplies the unbiased source to both the C libcore entrypoint and C++ Android clocks; the host pthread clock stays unchanged. The fetched libcore source is patched in place, preserving its upstream notices; no OpenJDK source is copied into this repository.

The new source-driven C regression compiles the actual fetched System_nanoTime body: three checks pass after the patch, and the same three fail on the original body. Actual Windows clock-domain brackets pass five checks; x64 runs and ARM64 compiles/links were checked. CI also retains the unchanged strict Java/ART bracket assertion. Physical sleep/hibernate remains unverified here.
