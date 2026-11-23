# Issue #178 JAVA-03: the shared Windows event wait

This is a small JAVA-03 leaf. It removes one empty wake per minute from the
system server's shared Windows event dispatcher. Connectivity and Bluetooth
observer retirement, their active reconciliation policies, and the whole-process
idle targets remain separate work.

## Production change and ownership

`WindowsHost.addListener` starts one server-owned daemon which receives the
pending native event bits and distributes them to its listeners. The dispatcher
has no housekeeping operation or deadline. Its 60-second timeout previously
returned zero, immediately entering the same wait again.

The dispatcher now calls `nativeWaitEvents(-1)`. The existing native
`gesher::devsvc::WaitEvents` treats a negative timeout as infinite and waits on
its condition variable. `Post` publishes pending bits under the native lock and
wakes that condition variable. Pending bits are checked before sleeping, so an
event arriving before the wait remains observable.

The window and dispatcher still belong to the server. Removing a subsystem's
last listener does not retire mandatory clipboard, clock, power, resume or toast
duties. This patch changes only the ordinary dispatcher's wait argument and its
comment. The foreground toast activation wait continues to use its finite
deadline; native alarms and fallback alarm behavior are unchanged.

## Verification and measured scope

`tools/test_windows_host_events.py` compiles the **complete production
WindowsHost.java** with small Android process/clock/log boundaries. Its JNI
fixture compiles the exact production native `Post` and `WaitEvents` bodies.

On Windows it additionally compiles the production event window, message loop,
startup and display-clock consumers. `WM_TIMECHANGE` is delivered to that actual
owned window. The Windows 2022 and Windows 2025 host-contract CI jobs execute
this fixture using JDK 21 and MSVC. Those runs are required target-host evidence;
a local cross-link is not their substitute.

On Linux the native window is explicitly substituted and the Win32 condition
variable is mapped to a host condition variable. Linux results prove the Java
owner/event flow and the native wait body under that boundary; they do not prove
Windows message delivery. Resume/power burst tests publish the corresponding
bits through production `Post`, rather than changing physical host power state.

The 25 assertions cover missing-native fallback, twelve concurrent listener
registrations creating one dispatcher, daemon/server lifetime, event delivery,
reentrant remove/add, callback failure isolation, a blocked callback followed by
coalesced resume/power bits, and a finite toast deadline amid unrelated events.
Alarm checks cover the unchanged Java fallback timeout/kick; the existing real
Windows device-service alarm gates remain active.

Measured on Linux with identical fixtures and a 65-second idle interval:

| Complete Java source | Empty timeout wakes | Actual event wakes during idle | Dispatcher CPU time during idle |
| --- | ---: | ---: | ---: |
| Main `8b316aebf09ab46d5407e955bf86ed748a1e739f` | 1 | 0 | 436,558 ns |
| Candidate | 0 | 0 | 0 ns reported |

The baseline passes the event/deadline cases and is rejected by the final idle
wake assertion. These are single-interval observations of one thread; the small
CPU difference is not a whole-process performance claim or a timing threshold.
Real external Windows events are permitted during the idle test and counted
separately from empty returns.

```sh
python3 tools/test_windows_host_events.py --java-home "$JAVA_HOME"
# Expected negative control: the final empty-wake assertion rejects main.
python3 tools/test_windows_host_events.py --java-home "$JAVA_HOME" \
  --source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f
python3 .github/scripts/test_automerge_workflows.py
```

The exact Win32 event/window/wait fixture also cross-links locally with
LLVM-mingw, warnings treated as errors. That compile uses the cached Linux JDK
JNI header with the x64 export declaration and is only a compilation check.
The CI path uses the actual Windows JDK JNI headers.

## Next demand-owned observer work

The complete JAVA-03 demand graph still needs explicit owners and retirement:

| Demand | Required lifetime |
| --- | --- |
| Remote network callback | Exact Binder registration and death recipient; undo failed admission, unlink exact removal, and fence replacement generations. |
| JobScheduler network constraints | A removable subscription retained for relevant pending/restored/running work, with immediate reevaluation on add/update/cancel/completion. |
| Legacy connectivity broadcasts | Matching live receiver demand distinguished from one-shot sticky queries; retain fresh state and manifest delivery obligations. |
| Native network watcher | Retained registration handles, explicit wake/stop, registration before snapshot and a generation fence around last-remove/concurrent-readd. |
| Bluetooth clients and operations | Receiver/client ownership plus pending discovery/pairing/SDP duties; one-time state queries do not own permanent observation. |

Callbacks must run outside ownership locks; a worker must never join itself
during reentrant removal. The existing active 15-second network and 10-second
Bluetooth reconciliation remains necessary until NLM property/cost/validation
and external Bluetooth pairing/adapter change coverage is demonstrated.
Registration failures require an explicit bounded recovery policy. None of
these demands is removed by this leaf. Ten-minute no-client traces and the
broader eight-hour/foreground acceptance remain outstanding.
