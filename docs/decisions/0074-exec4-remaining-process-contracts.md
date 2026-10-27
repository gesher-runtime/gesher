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
