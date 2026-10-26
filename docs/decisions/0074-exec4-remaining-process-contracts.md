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
