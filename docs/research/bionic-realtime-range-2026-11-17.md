# Bionic public realtime signal range

PR120's actual Windows2022/2025 ABI runs aborted on the missing
`__libc_current_sigrtmin` after three ordinary setjmp checks. The public
`SIGRTMIN` macro calls this export; substituting a fixture constant would hide
the missing runtime API. The repair implements and registers both public range
functions in the shared Bionic provider, preserving errno and mutable state.

## Android contract

Pinned Android16r4 [signal.h](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/include/signal.h)
defines `SIGRTMIN`/`SIGRTMAX` as calls to these functions. The
[minimum](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/__libc_current_sigrtmin.cpp)
adds the reserved count to the kernel minimum; the
[maximum](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/__libc_current_sigrtmax.cpp)
returns the kernel maximum. The
[reserved header](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/platform/bionic/reserved_signals.h)
reserves ten signals. The
[x86 header](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/kernel/uapi/asm-x86/asm/signal.h),
[generic header used by ARM64](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/kernel/uapi/asm-generic/signal.h)
and [signal types](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/include/bits/signal_types.h)
give kernel bounds32–64, hence public bounds **42–64** on both supported LP64
guest ABIs. Host CRT/glibc ranges are not substituted.

## Dispatch and validation

The existing x64 ELF resolver binds the ordinary System V functions directly.
The ARM64 loader binds the same provider through its guest-to-host trampoline.
No guest override is needed. The original export repair left mask handling,
queues and delivery unchanged. The source-QA follow-up below also corrects
the shared provider's reserved-signal mask filtering; it does not implement
PR79's delivery work.

The same eight-check fixture is included in the existing x64/ARM64 `libc_test`
guests. It exercises actual macro imports and `dlsym` resolution, exact bounds,
errno preservation, repeat queries and endpoint signal sets. Windows tracks40
and47 require its dedicated marker alongside whole-library/loader success.
All existing realtime setjmp/mask cases and their required marker stay intact.

The identical standalone fixture passed all eight checks on actual API36
x86-64 Android, exit0, with its device SHA256 matching
`16564ced4684f067eb4c99c0a13693c63625b4722221b020d1dda8ac665575e1`.
This is Android reference evidence only. The full production signal translation
unit cross-compiles for Windows x64/ARM64, and both whole guest ELFs compile.
Actual candidate Windows execution through both loader routes remains required.
The narrow export repair does not complete N20 delivery or N15 callback jumps.

## Source-QA follow-up: reserved masks and prerequisite integration

The jump helpers call this shared provider. Independent review found that
`PthreadSigmask` excluded only SIGKILL and SIGSTOP, allowing normal public
calls to unblock Bionic's timer signal or block its other reserved signals.
The repair follows Android 16 r4 commit
[`f22516cbc67e81c13cf943ce02f01b8929a98726`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/):
[`sigprocmask.cpp`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/bionic/sigprocmask.cpp)
validates a supplied operation and applies
[`filter_reserved_signals`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/platform/bionic/reserved_signals.h)
to the **requested** set before applying BLOCK, UNBLOCK or SETMASK.

BLOCK and SETMASK add signal 32 to the request and remove 33–41. UNBLOCK
removes 32 from the request and adds 33–41. A query with no new set ignores
`how`. Filtering the final mask instead would incorrectly rewrite existing
raw state for BLOCK/UNBLOCK. No signal queue, handler dispatch, thread creation
or internal receiver-thread policy is changed. The operation uses only local
integer state and adds no allocation or persistent storage.

`tools/test_bionic_signal_masks.py --sanitize` executes the actual production
mask/error entry bodies with explicit thread-state and delivery boundaries.
Its 46 checks pass on the repair; the original source fails 9 checks under
the same assertions. It covers all reserved bits, public lower/upper bits,
requested-versus-result filtering, old-set output, input preservation, query
semantics and the distinct pthread/sigprocmask error conventions. The gate is
registered in general CI.

The actual x64/ARM64 guest range fixture retains the original 8 export checks
and adds 13 mask API checks, including signal 64 and new/old pointer aliasing.
Its production entry always requests the full signal 64 endpoint. A standalone
reference may explicitly select another upper-half signal when its CPU/kernel
emulator cannot represent64; such a result is a bounded reference and cannot
qualify the signal 64 endpoint. The endpoint is never silently skipped.

The available NDK r27c static Bionic reports SIGRTMIN=41 and retains signal 41
in its requested mask, unlike the pinned Android 16-r4 contract. Its negative
reference results are retained and are not presented as Android 16 validation.
QEMU linux-user also has the separately documented signal 64 limitation.
Neither limitation changes the production requirement of reserved 32–41 and
public 42–64.

The repaired composition integrates approved PR128 and its PR123/131
prerequisites, retaining both the setjmp/range additions and the production
binary128/HostCall work. The only merge resolutions combine required includes
in `guest_libc.cc` and `libc_test.c`, and retain current main's helper metadata
completion text. No callback-transfer implementation is added: legal jumps
across suspended native callbacks remain a separate, documented failure.

Fresh source checks and cross-compilation do not constitute a new Windows
or shipped ART execution result. Earlier Windows receipts remain tied to
their earlier source; required CI must execute the integrated candidate.
