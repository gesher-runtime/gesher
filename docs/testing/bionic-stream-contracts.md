# Bionic stream contracts

The wide stdio implementation targets the observable contracts in Android
`android-16.0.0_r4`, with Windows CRT locks, file objects and byte I/O underneath.
The primary references are Bionic's
[vfwprintf.cpp](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/stdio/vfwprintf.cpp),
[vswprintf.c](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/upstream-openbsd/lib/libc/stdio/vswprintf.c),
[printf_common.h](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/stdio/printf_common.h),
and [stdio.cpp](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/stdio/stdio.cpp).

## Formatting

`printf` conversions containing `%n` terminate with the Bionic FORTIFY
diagnostic, for both narrow and wide output. The separate `scanf` `%n`
conversion remains supported.

Wide stream output commits completed literals and fields as they are processed.
A later conversion error preserves the preceding output and marks the stream
error. Wide character and string fields do not require a complete UTF-32 result
buffer. `%C` and `%S` select wide character and string arguments; `%D`, `%O`, and
`%U` select the Android long-integer aliases.

Wide buffer output first encodes to a multibyte buffer and then decodes through
the production Bionic `mbsrtowcs` converter, as Android does. Consequently,
encoder and decoder acceptance can differ, and an embedded NUL can stop the
copy before the returned formatting count. Zero capacity reports `EINVAL`;
truncation reports `EOVERFLOW` and NUL-terminates the available prefix. Allocation
failures inside the wide formatting entry points report `ENOMEM`.

Creating host stream metadata can also fail. The stdio entry points report
`ENOMEM` and their failure result before consuming input, writing output or
changing pushback. `fwide` returns zero if the stream could not be oriented.
Status and orientation queries, `clearerr`, and existing metadata do not require
a new allocation; a failed first allocation leaves the stream available for
retry.

These contracts do not establish complete printf parity. Positional formatting,
wide scanning, the inherited numeric conversion implementation, and other
unimplemented conversions require their own qualification.

## Stream lifetime

The POSIX close/reopen adapters report whether the old CRT stream was actually
retired. Bionic removes its orientation, pushback, conversion state and error
record while still holding the CRT stream lock. A descriptor-transaction failure
before close preserves the live record; a failure after retirement does not
restore it. `pclose` preserves its child association and returns immediately if
the pipe was never closed, allowing a retry without waiting on an open writer.

This change does not qualify the inherited `freopen(nullptr, mode, stream)`
implementation: it currently returns the stream without performing Android's
descriptor-based reopen. Named-path reopen and the distinction between failures
before and after retirement are the scope of these lifetime tests.

## Validation boundaries

Run the production-source lifetime checks on Linux with:

```sh
python3 tools/test_stdio_lifetime_contracts.py --sanitize
python3 tools/test_stream_device_lifetime.py --sanitize
bash tools/ci/test-freopen-reference.sh
```

The first two tools extract actual production functions. Their host boundaries
replace Windows file locks and descriptor-transaction admission/completion;
Linux performs real file open/close/reopen. The pclose test counts the process
wait boundary rather than launching a Windows child. These are neither UCRT
slot-reuse tests nor Windows record-lock fault injection.
The lifetime tool also injects real C++ allocation failures into the extracted
metadata registry and stdio entry points. It verifies that failures preserve the
live stream and return before I/O; its wide codec functions are counting
boundaries, so this check does not establish conversion behavior.

`bionic_stdio_contract_test` exercises exported Bionic symbols on Windows. It
contains CRT lock witnesses, wide I/O, buffer errors, partial stream output,
aliases, and bounded fatal children for printf `%n`. The Windows ARM64 native PE
cannot directly exercise the translator's variadic calling convention, so those
variadic cases require the guest/translator suite. On Linux the same executable
is a glibc control; its success does not establish Android behavior. On Android
it uses Bionic and enables the Android-specific assertions.

Cross-compilation, Linux sanitizer checks and static Bionic binaries under
QEMU do not replace Windows x64/ARM64 execution, UCRT contention, guest assembly
entry points, translator/ART integration or end-to-end application testing.
