# 0074. Remaining executable, shell, console and log contracts

Status: proposed, owned by [exec4](../handoff/tracks/exec4.md).

ADR 0060 accepts a verified shell/tools/property/log/TLS slice; it does not
establish full POSIX/Linux process compatibility. Complete the explicit
exec-replacement, console, event/logd, TLS, fork/static-executable and property
policy/notification contracts with real runtime callers and correct ownership.
Coordinate filesystem permissions with native3 and Android identity/Binder
semantics with services3/binder2. Preserve the tested shared property model.

Acceptance requires actual semantic tests and full Windows/package/e2e CI,
not an export inventory or a shell-only representation of missing behavior.

## Verified process-exit results

The package's cached exit result is bound to a unique start sequence and
actual PID. A new start clears the old result; late/conflicting callbacks
cannot publish an older success. The pending/death observer handoff keeps
the destructive native exit query under its ownership lock. Launcher fails
when the latest result remains unknown instead of manufacturing success.

Accepted as a bounded slice in full CI 40455262527 at
29fa0bdf665a2cc5ee707524e7edd2a670391451, merged35fba0ab. Both Windows
suites have0 failed tests, actual ART generation/PID controls and the real
same-package app0/23/0 versus launcher0/1/0 sequence. Installed package and
both ten-app e2e pass. The merged code equals the verified source.
This is not a cause attribution for the older UIA/native-crash reports.
Untracked adopted exits remain unknown; the remaining exec4 contracts and
this aggregate ADR's proposed status stay open.

The first priority is a sporadic own-process native crash observed in
Windows4 diagnostic CI40417632512 (single-process exec3 APK on Windows-2025).
It follows the read-mode popen assertion and precedes posix_spawn's next
assertion. Bounded opt-in first-fault context and actual-operation phase
markers will establish a location before attributing a cause or accepting a
fix. Previously verified exec3 snapshots and failed logs remain preserved.

## Live environment data

The ELF shared-library environment must bind to the live CRT pointer slot.
A copied array pointer can become stale when host startup changes its
environment after shim initialization, including Java Os.setenv or property
session binding. Both `environ` and `__environ` use the same data address;
refreshing a snapshot only in guest setenv cannot observe host mutations.
[Microsoft's CRT documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/putenv-s-wputenv-s)
specifies that both narrow and wide setters can change these array pointers.
The existing narrow CRT representation is retained. The executable launcher
still owns a separate initial UTF-8 array, whose mutation behavior is an open
contract; this correction does not replace that array with an ACP encoding.

The prepared regression first checks live slot identity without traversing
old memory, then tests actual relocated ELF imports, host narrow/wide changes,
guest mutations and a real posix_spawn child. It also runs through the two
existing ART APK modes after Java host mutations. Compilation and execution
remain CI-only; source inspection has not established that this defect caused
the earlier sentinel crash. The frozen diagnostic remains active for that
separate causal evidence.

The remaining source audit confirms that console input still reaches a
synchronous ReadFile (only pipes are polled), both process-spawn helpers force
CREATE_NO_WINDOW, and shell exec still starts/waits for a different process.
The log store records C-string tag/text rather than arbitrary binary payloads
and logd framing. These are concrete follow-up boundaries, not implemented or
verified compatibility. Keep them separate from the live-environment candidate.

## Verified live-environment slice

Full CI40430496516 attempt2 passes at exact source
9f250803cd184e05d024733da8296410007de76f: all 28 jobs completed, 22 success
and six intentional skips. Actual logs were read for both zero-failure Windows
suites, every exec3 Check/57 shell cases, the new host CRT/NDK/spawn regression,
both host-mutated APK modes, package install/oatdump/window/input/cleanup and
both nine-app e2e runs (0 unexpected failures, required Puzzles/TIC-80).
Attempt1 failed only a framework curl HTTP/2 transport (exit92); failed jobs
were retried after that run completed, on the identical source and ref.

The earlier diagnostic610 also passes both APK modes and completes every
spawn/stdio phase marker without reproducing the crash. It is red only for
two Windows-2025 external badssl timeout Checks; Windows-2022, package and
both e2e runs pass. That non-reproduction and the green correction do not
establish the precise cause of the older sentinel crash. Only the live shared-
library environment correction is verified here; the initial executable UTF-8
array and all remaining process/console/log/TLS/fork/property contracts remain
open. Root owns latest-integration acceptance and merging.

## Verified action-registration slice

Full40466021634 at160e25e8 verifies test74's actual x86-64/arm64 NDK callers
on both Windows runners:15 negative descriptor registrations return EBADF
without changing errno or corrupting the action list, positive unopened
descriptors register, and real spawn/spawnp children wait with exit42.
Both full suites0, package23 and both ten-app e2e0 were independently read
by root; merged6297e80a matches the verified non-doc tree. This accepts only
registration boundaries. Ordered child-side action execution/failure127 is
the separate proposed ADR0076, while the aggregate ADR0074 remains open.
