# Exec4: fresh process exit results

Root owns wip/exec4-exit in C:\work\gesher-exec4-exit under ADR0074.
Test70 is reserved. No integration/main/release acceptance is claimed.

## Context

The server removes a dead Binder process before its asynchronous OS-exit
query completes. Its package-only cache retained an earlier process's zero
code across new starts, so the launcher could report that stale success.
The newly hardened Web3 marker alone could not establish result freshness.
This source interleaving does not prove the cause of an earlier UIA stall.

## Status and next steps

Source review found two additional issues before acceptance: the native
exit query consumes its handle, so pending-process ownership, query and
publication now stay under one short server lock before attach can hand
off to the death observer. The abnormal runtime assertion also requires
the launcher's exact exit1, rejecting the timeout sentinel. No result
from the first scoped framework build implies runtime acceptance. Scoped
framework CI40452236687 at49e20a1 failed in the new component fixture:
`exited("com.gesher.other", 4, 17)` omitted its bound PID102; javac reported
required String,long,int,int but found String,int,int. The fixture now
supplies PID102. Production gesher-app.jar compiled before that failure.
Integration f17d03b1 (Media3/full CI40448521754 atdd06d296, documentation
only above its matching code) is merged before the corrected CI run.

Prepared source only: ProcessExitStatus binds results to a globally unique
start sequence and actual PID, clears the old result at every new start,
and rejects late/duplicate-conflicting callbacks. Both spawned and adopted
package-assignment paths obtain fresh generations. The launcher fails when
the latest OS result remains unknown instead of returning success.

The actual helper is tested under ART with stale zero/failure, pending PID,
PID reuse, callback ordering and cross-package concurrency controls. A real
APK/launcher sequence runs the same package normally, exits it with code23,
then runs it normally again, asserting fresh child and native launcher
exit statuses. The package is a separate CI app with one build/copy line.
All existing timing and app assertions stay intact.

Only source/PowerShell/YAML/XML parsing has run locally. Every Java/native
compile and runtime check goes to CI. Before final full CI include current
integration and coordinated verified environment/deterministic TLS sources,
review the shared process-lifecycle changes, then require both Windows zero
summaries, all scoped PASS lines, package and both required e2e logs. Adopted
processes without a tracked OS exit remain explicitly unknown; no success
is manufactured from an absent result. Other exec4 contracts remain open.
