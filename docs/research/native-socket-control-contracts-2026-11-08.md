# Native socket control contract, 2026-11-08

Baseline `a4dd7242cfed1a08cd8761ae896d63faf7b8cf63` explicitly ignored
`sendmsg` ancillary data in `compat/posix/src/socket.cc`, sent the ordinary
payload, and reported success. A peer expecting accompanying file descriptors
or credentials could receive a message that the sender believed was complete.
That can break a protocol, exhaust retries, or reach a caller's error path.
No specific APK crash is claimed from this regression alone.

## Corrected behavior and remaining limitation

After the guarded header copy, `sendmsg` now returns `-1/EOPNOTSUPP` whenever
`msg_controllen` is nonzero. It does so before building buffers or sending any
payload. A non-null control pointer with zero length remains an ordinary send.
The existing `sendmmsg` adapter propagates an initial failure, or returns only
the completed prefix when a later message is rejected; unprocessed lengths
are untouched.

This makes the missing capability observable; **it does not implement Android's
ancillary transport**. In particular, `SCM_RIGHTS`, credentials and network
control messages still require a real descriptor/ownership and transport
implementation. `recvmsg` still reports zero ancillary length, because the
current backend receives payload only. Apps that require descriptor passing
remain unsupported and can fail, even though false transmission success is
removed.

Real Android uses `sendmsg` to carry descriptors; for example,
[bionic's profiling handler](https://android.googlesource.com/platform/bionic/+/a9750eda0/libc/bionic/android_profiling_dynamic.cpp)
sends process mappings and memory descriptors in `SCM_RIGHTS`. The chosen
unsupported-operation error is also used by socket implementations whose
transport cannot pass access rights; see
[IBM's sendmsg error contract](https://www.ibm.com/docs/en/i/7.5.0?topic=s-send-message-over-socket-sendmsg).
The guard is an explicit compatibility boundary, not a claim that Android
rejects otherwise valid ancillary messages.

## Verification

`tests/native/socket_control_test.inc` runs in the existing x86-64 and translated
ARM64 ELF libc suites. It uses independent peer reads on both stream and
datagram socket pairs, so a function that merely returns the expected error
after sending data cannot pass. It verifies no peer payload on rejection,
ordinary scatter/gather sends, the zero-length control case, and both first-
message failure and partial completion in `sendmmsg`.

```sh
python3 tools/test_socket_control_contract.py --sanitize
git show a4dd7242cfed1a08cd8761ae896d63faf7b8cf63:compat/posix/src/socket.cc > /tmp/socket-before.cc
python3 tools/test_socket_control_contract.py --source /tmp/socket-before.cc --sanitize
```

The host check compiles the actual production `sendmsg`, buffer helpers,
bionic adapters and public registrations verbatim. It uses the real production
buffer declarations. The envelope supplies host equivalents for the guarded
copy and payload transport, and uses the Linux kernel's real socket pairs as
the independent peer. It does not execute Winsock, descriptor registration,
Windows guarded-copy faults or Windows cancellation paths.

The corrected source passes **40 assertions** with ASan/UBSan. The unmodified
baseline fails **17 assertions** across the two socket types, including
observable forbidden payload and incorrect batch counts. The shared fixture
also passes C11 compilation with warnings treated as errors. The local run
uses `ASAN_OPTIONS=detect_leaks=0` because this executor does not permit
LeakSanitizer's `/proc/*/task` inspection; it is not a leak-check result.
The full Windows and translated ARM64 suites remain integration gates.
