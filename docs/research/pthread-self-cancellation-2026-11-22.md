# Current-thread cancellation and TLS destructor logging

## Observed failure

Canonical CI run `44969729540`, attempt 2, failed the services shard on both
Windows 2022 (`137062710973`) and Windows 2025 (`137062710951`). The checkout was
synthetic merge `d6159a8e21f400395f90ccaf35a1c717755eed5e`, tree
`70b9246a99863ebda86e2166e02b0babc12cf485`, with PR source
`37f463c2bf4776830e319248703e701a24f94181` as its second parent. The relevant
Binder, stdio and pipe paths were identical to that PR head.

The libbinder probe printed `all passed` after terminating and waiting for its
children, then remained alive until the unchanged 120-second process timeout.
This was the `WaitForExit` timeout, not the separate redirected-reader timeout.
The last messages came from the client's main and Binder pool threads after
the broker closed their connections. AOSP's pool correctly leaves on `EBADF`;
its exiting thread then runs `IPCThreadState::threadDestructor`, which can log.

## Lock cycle

The original llvm-mingw publisher uses mingw-w64 commit
`a3d93999ef0521681d45e445c39964a7af0f593f`. Its `thread.c` SHA256 is
`c2fcaa54269f0e773069aeeec8212cac71f417483bf65d5e75f100311df4080b`.

`pthread_create_wrapper` holds `mtx_pthr_locked` while invoking TLS destructors.
A destructor which logs therefore acquires the registry before the stream lock.
Conversely, ordinary stdio holds the stream's CRT/shared locks while the pipe
adapter calls `pthread_setcancelstate`. That setter already has the current
thread record, but `test_cancel_locked(t->x)` looks it up again through the
global registry. This creates a registry/stream lock inversion, including when
the requested cancellation state is disabled.

The repair passes the already-owned current record to the private helper from
its only two callers, `pthread_setcancelstate` and `pthread_setcanceltype`.
Both still hold the same per-thread `p_clock`; validation, returned old state,
pending cancellation and asynchronous delivery are unchanged. The current
thread cannot be retired while it is executing either setter. No public ABI,
private record layout or test-specific runtime branch is introduced.

## Supplier and product evidence

`tools/toolchain/prepare_pthread_cancel.py` creates the separately named
`publisher-cancel` production overlay from the exact original compiler archive
and pristine upstream source. It applies only
`pthread-cancel/self-cancellation.patch`. The existing selected-minimum two-patch
experiment, its pins and its factory remain unchanged; the shared publisher
cache is not modified. Canonical `ci/build` selects the new role explicitly.
Other component/reference workflows keep their existing selection.

The factory checks input and output source hashes, all upstream public headers,
static symbols, DLL exports/imports, the original Windows SDK profile, and the
actual 480-byte private record layout. It binds the installed archive member
and DLL symbol owner to their actual source compile outputs. The product gate
then checks the real link-map owners of both cancellation setters in
`libbinder_probe`, `stdio_ofd_test`, `servicemanager` and `dalvikvm`. Separate
receipts and raw supplier evidence are retained as `pthread-cancel-runtime`.

The repaired `thread.c` SHA256 is
`b1222d30268e18662bc1bd7bc5c9b10b89382e579b19656255fc251412805b96`.
The patch SHA256 is
`0c7262eb0f9bae84ae7a2db164b62fd122f8e80167265431a6bbcc92bba32c4a`.

## Regression and limits

`stdio_ofd_test --tls-exit` creates a real Windows pipe-backed `FILE`. Main
first establishes its current pthread record, then locks the stream before
starting a pthread. The pthread returns with a TLS
destructor which signals entry, then writes to that same stream. Main writes
while retaining the stream lock, then releases it and joins the worker. The
test verifies actual bytes from both writers. The event establishes the lock
order without timing sleeps or private supplier hooks.

The lifecycle shard runs this as a separate child under the existing bounded
process harness. A timeout remains a failure. The original libbinder integration
probe and its timeout are unchanged, so successful assertions followed by a
stuck teardown cannot become a pass.

Local evidence includes an exact-source host lock-order reproducer (the original
setter deadlocks; the repaired setter completes while preserving state/type,
invalid-input and cancellation branches), a full x64 Windows cross-build of
the separate supplier, and compilation of the actual native stdio fixture with
the product's flags. An explicitly small CMake/Ninja fixture exercised all four
link launchers and evidence replay, and the real observer helper cross-build
used the new verified compiler role. Mutated receipt, compiler and captured
link evidence were rejected by 31 negative controls. These small hook fixtures
are not the four actual Gesher products. Host modelling and cross-compilation are not execution on
Windows. Both the native TLS regression and the original libbinder gate must
pass on Windows before treating this runtime repair as qualified.

## Primary source locations

- [Pinned winpthreads thread implementation](https://github.com/mingw-w64/mingw-w64/blob/a3d93999ef0521681d45e445c39964a7af0f593f/mingw-w64-libraries/winpthreads/src/thread.c)
- [Pinned AOSP IPCThreadState](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-16.0.0_r4/libs/binder/IPCThreadState.cpp)
- [Canonical failed CI attempt](https://github.com/gesher-runtime/gesher/actions/runs/44969729540/attempts/2)
