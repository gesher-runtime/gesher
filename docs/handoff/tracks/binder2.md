# Track brief: binder2

Binder transport contracts left open by ADR 0015 and the input-channel
restriction in ADR 0027. Reserved ADR: 0068. Branch `wip/binder2` starts from
integration after aot2; use a dedicated worktree under C:\work.
Follow track-rules.md and read compat/binder, compat/posix/src/binder.cc,
the binder UAPI and current FD transfer/Parcel tests before implementation.

Required work:
- Audit BINDER_TYPE_FD/FDA against existing DuplicateHandle transport and
  real close/ownership/error behavior. Surface Bitmap/memfd transfer already
  passes in ci 40307919084; do not call all descriptor transfer absent.
- Close scatter-gather BINDER_TYPE_PTR/HIDL buffer fixup and ownership limits,
  with bounds/error tests and real cross-process transactions.
- Implement the remaining BR_TRANSACTION_SEC_CTX, freeze and transaction
  priority behavior against the Windows process/thread model, with actual
  observable results and a documented identity model.
- Make polling a binder fd obey readiness, cancellation and concurrent waiter
  rules. Coordinate epoll internals with native3.
- Verify an InputChannel/socket descriptor sent across binder to another
  process (ADR 0027), including independent process lifetime and closure;
  coordinate socket handle duplication with web3.

Test real binder clients, broker and service processes on both Windows
runners, retaining package, application lifecycle, descriptor and both e2e
checks. No local AOSP build or WSL. Merge current integration before final
full CI and cite actual PASS lines; protocol support is not established by
accepting a command without implementing its effect.

## Status and next steps

Reserved on 2026-10-26 during the post-aot2 gap audit. Unstarted. Existing
descriptor support is verified only in its cited contexts; the remaining
contracts above are open, pending source audit and executable evidence.
