# Guest isolation disposable experiment

Worktree: `C:/work/gesher-guest-isolation-probe`, branch
`wip/guest-isolation-probe`, integration base `f1550540`.

The initial `065f66b0` checkpoint lacked host IPC, independent trusted code/config
write denials, an explicit allowed resource challenge, strict receipts and final
cleanup verification. Implementation `15797ccc` was source-reviewed and corrected
in `9948b68cd1b6b1121576b7720ad9a45baba0e421`. The sole integrator source peer
accepted that corrected source before its first push. Lightweight Python/YAML
and embedded PowerShell parsing and `git diff --check` passed locally. No local
native/Java/C# compilation or fixture was performed.

Current changed-source scoped run: `40783239100`, attempt 1. The build has passed
the pinned upstream archive hash check and the actual llvm-mingw -Werror fixture
compilation. Both Windows runtime controls and terminal receipts are pending.
Watch uses `gh run watch --exit-status --interval 120`. No unchanged red retry
is authorized; retain actual failures and make a concrete correction first.
The track's observer session `48743` was stopped with Ctrl+C (observer exit 1,
not CI cancellation) at the manager's request. The sole integrator now owns the
single watcher and terminal runtime/receipt audit; this worker has no observer.

The fixture starts its child suspended with SECURITY_CAPABILITIES, zero
capabilities, an explicit two-handle allowlist and atomic JOB_LIST membership in
a kill-on-close job with one active process. Both parent and child check the
actual AppContainer SID. Each process/thread right is tested independently with
host-positive controls. Separate readable existing code/config markers must deny
writes and remain unchanged. A live listening host-only pipe must deny opening
after the host successfully opened it. Explicit resource input/output pipes
carry a fixed challenge and exact observations. Owned child cleanup requires a
signaled process within the deadline; unique files/directory/profile cleanup
errors fail. No pending overlapped I/O is left on the host listener.

The workflow uses the qualified existing RunTimed supervisor, current-run and
attempt producer archive receipts, exact member inventory, source/executable
hashes and compiler invocation/binary/version provenance. The upstream toolchain
archive SHA256 is pinned from release asset `618062377`; no unverified toolchain
cache is consumed. Receipt mutation controls run before either Windows launch.

This is a disposable Win32 capability experiment only. Production guest launch
still uses an unrestricted same-user host boundary. An unrestricted same-user
process remains trusted. No production guest isolation, Binder authentication,
secondary AOT, integration, full-suite, package, main or release qualification is
established by this component. The production security requirement remains open.
