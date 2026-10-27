# Web3 native-window teardown diagnostic

## Owner checkpoint

Work is stopped at the owner's explicit request; no further implementation,
dispatch or integration action follows this checkpoint. Diagnostic
40531106587, exact source5bd1732e0fa8de8012a560b142908834d108d47d, attempt1,
is complete green with three jobs. Actual observer-build122283828833 and
Windows2022/2025 logs122283985963/122283985915 were read. The real held-process
control yields exact PID/TID WCT records and ntdll/WaitForSingleObject native
frames on both hosts. All nine supplying artifacts are pinned. Each host has
14 Web3 PASS, five VM0, six launcher0, six fresh child0 and0failed.
There are ZERO original teardown deadline failures: the hang did not recur,
so no failing app stack, cause, fix or new full acceptance is established.
Do not rerun unchanged to hunt for a green result.

Completed watcher: interval120, local log
`C:/work/gesher-ci-review/web3-native-diagnostic-40531106587-watch.log`.
Metadata and three actual plain logs use that same local prefix. The exact
producer is accepted full40499454478/source694a4cd6b067f1deea87015bceb2ad2160260154.
The next session should retain old405123/96b raw failures, preserve this
observer for the next meaningful failing source, and investigate actual
native ownership/lifetime before claiming resolution. No own CI is active.

The accepted source remains full40466021634/160, followed by coherent
Net4 full40499454478/694. The later AOT full40512344714/96b is red on2025:
device A11y completes its actual UIA/DOM assertions and Activity destruction,
then stops after `nativeWindow.close enter`. Its app PID3140/start-sequence4
stays alive; the next two NetActivity launches reuse that process and lack
their new markers. This is a recurrence, not a proven AOT or UIA cause.

Source trace reaches NativeWindow's decStrong, Win32Surface destruction and
Window destruction. The last unregisters routes, posts WM_APP and joins the
owner STA; the STA destroys its HWND and uninitializes COM. The existing log
has no native PC/stack distinguishing these phases. Java UIA raising already
uses an executor; the old synchronous-raise hypothesis remains withdrawn.

The new independent web3-probes workflow pins the complete successful
40499454478/694 producer, attempt1, repository, supplying jobs, artifact IDs,
digests and source equality. All ART, Java and boot-image bytes are from that
accepted run; artifacts download only inside CI. Only a separate observer
is compiled anew, using existing thread_watch.cc native-stack/WCT machinery
with llvm-mingw/Werror and no AOSP build. Its held-process control requires
an actual native stack and exact PID/TID wait-chain record on each Windows.

The real test54 still runs once with its original assertions and deadlines.
After its original60+60s teardown failure is recorded, a bounded external
observer reads at most40 threads/40 frames per process, without sending the
app another window message or changing teardown. It checks SuspendThread
success before balancing ResumeThread. The prefixed diagnostic process
runner copies Root's exact ec0f003c RunTimed body; process timeout25s,
post-kill5s and each pipe5s remain explicit. The common helper is not edited.
Microsoft documents that synchronous WCT cannot be cancelled, hence this
separate child lifetime bound:
[Wait chain traversal](https://learn.microsoft.com/en-us/windows/win32/debug/wait-chain-traversal).

No hang fix, full acceptance or expanded accessibility coverage is claimed.
A passing diagnostic with no recurrence would provide no causal resolution.
Actual stacks, ownership/lifetime correction, focused counterfactuals and a
complete coherent full remain necessary. Window/Web4/hardware/insets limits
from the accepted evidence remain open. All builds/execution are CI only.
