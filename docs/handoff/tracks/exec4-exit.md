# Exec4: fresh process exit results

Root owns wip/exec4-exit in C:\work\gesher-exec4-exit under ADR0074.
Test70 is reserved. Merged as35fba0ab after the full CI verification below.
Other exec4 contracts and the final main/release gates remain open.

## Context

The server removes a dead Binder process before its asynchronous OS-exit
query completes. Its package-only cache retained an earlier process's zero
code across new starts, so the launcher could report that stale success.
The newly hardened Web3 marker alone could not establish result freshness.
This source interleaving does not prove the cause of an earlier UIA stall.

## Status and next steps

Full CI40455262527 at29fa0bdf665a2cc5ee707524e7edd2a670391451 passed
attempt1:28 jobs,22 success/6 intentional skips. Root read all job metadata
and actual test2022/122039787831, test2025/122039787913,
package122039787753 and e2e122040311361/122040311469 logs.
Both Windows summaries are0 test(s) failed, with these actual results:

- PASS exec4 exit: only the latest bound PID/start can publish launcher success.
- PASS NormalActivity: fresh app0 and launcher0.
- PASS AbnormalActivity: fresh app23 and launcher1.
- PASS NormalActivity again: fresh app0 and launcher0.

The actual component runs under ART, including old/unbound/mismatched PID,
PID reuse, conflicting callback and concurrent stale/cross-package controls.
Package has23 installed oatdump/image/fonts/window/input/uninstall passes;
both e2e run all ten apps, four transport controls and association/shortcut
launches with0 unexpected failures. Existing live-environment/time/media,
shell/process, original boot-profile size/startup and app-image gates remain
enabled and passed. No retry or deadline/assertion relaxation.

Frozen head7d219f59 adds documentation only. Root merged it into integration
as35fba0ab and confirmed the non-documentation tree exactly matches29fa0bdf;
no additional integration CI is needed under rule-g. The copied-brief
add/add conflict retained this accepted result and all source history.
Windows4/Web3 must then run on this prerequisite. This fixes exit-result
freshness, not the cause of an earlier UIA stall; untracked adopted OS exits
remain explicitly unknown/failure. Child action/fork/exec4 gaps remain open.

### Source and scoped-build history

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

Corrected scoped framework CI40454416483 at2a9c409 passed both jobs. Root
read the actual framework log122024336684: gesher-app.jar, graphics-tests.dex
including the corrected component fixture, and exitstatusapp.apk all built.
This is compile/package preparation only; no runtime pass is claimed. Current
integration808ea746 (documentation only above f17d03b1) is merged before
dispatching the first full CI of the corrected process-lifecycle source.

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
