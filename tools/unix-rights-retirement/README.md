# Queued SCM_RIGHTS on receiver close

These standalone diagnostics cover the receiver cleanup part of
[issue #56, N43](https://github.com/gesher-runtime/gesher/issues/56).
Each puts the sole writer of a nonblocking pipe in a Unix socket's receive queue
and closes the original writer. The reading end must report `EAGAIN` while a
receiver alias is alive and EOF after the last alias closes. No application
receives the queued writer. `last_close` uses `dup`; `transferred_close` sends
the receiver itself over a separate socketpair and checks its lifetime both
during transit and after reception. Both cover `SOCK_STREAM`, `SOCK_DGRAM`, and
`SOCK_SEQPACKET`.

## Run

Run from the repository root on Linux.

```sh
cmake -S tools/unix-rights-retirement -B build/rights-linux
cmake --build build/rights-linux
build/rights-linux/last_close
build/rights-linux/transferred_close
```

For Windows, cross-build with llvm-mingw and the pinned AOSP inputs used by
the POSIX component.

```sh
python3 tools/pipe-component/prepare-inputs.py "$PWD/build/rights-aosp"
cmake -S tools/unix-rights-retirement -B build/rights-windows-x64 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-llvm-mingw.cmake" \
  -DLLVM_MINGW=/path/to/llvm-mingw -DAOSP_DIR="$PWD/build/rights-aosp" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rights-windows-x64 --parallel
```

For ARM64, use `toolchain-llvm-mingw-arm64.cmake` and a separate build
directory. On Windows, pass the directory containing the three executables to
the launcher. It starts an isolated broker and gives each probe 20 seconds.

```powershell
powershell -NoProfile -File .\tools\unix-rights-retirement\run-windows.ps1 -BuildDirectory .\build\rights-windows-x64
```

## Observed behavior

| Target | Checks passed | Read after last receiver close |
| --- | ---: | --- |
| Ubuntu 24.04 ARM64 | 30/30 | `0` (EOF) in both probes |
| Windows 11 x64 binaries | 24/30 | `-1/EAGAIN` for each socket type in both probes |
| Windows 11 ARM64 binaries | 24/30 | `-1/EAGAIN` for each socket type in both probes |

The Windows probes link the production Gesher POSIX library and run against
production binderd. The x64 binaries run under emulation on the Windows ARM64
VM. The six failing Windows checks per architecture show that the broker
continues to own the queued pipe writer after the receiver closes.

## Protocol prerequisite

The [current descriptor broker design](../../docs/decisions/0088-unix-socket-credentials-and-descriptors.md)
tracks stream groups by loopback ports and packet groups by their per-message
tokens. Neither binds a group to the lifetime of the receiving endpoint.
Socket descriptors carry a Winsock handle and scalar metadata through Binder
and SCM_RIGHTS. An imported socket begins with its own process-local
descriptor count, and an endpoint in transit can have no application
descriptor at all.

Receiver cleanup needs a stable endpoint identity and a lifetime witness that
covers local aliases, transfers staged in the broker, and imported aliases.
Queue retirement must run when that endpoint loses its last reference and
serialize with deposit, claim, and cancel. The witness must observe resource
retirement without keeping the receiver alive. Socket references retained
only by unreachable queues also need lifecycle accounting. The N43
receiver-cleanup item remains open.

## Socket identity probe

The same component build produces `socket_identity`. It checks the requested
Unix domain, type, unnamed endpoint addresses, and an absent per-message source
address before and after aliasing and transfer. The transferred endpoint
receives real payloads after the original descriptor is closed. Run the probe
directly on Linux. On Windows run it under a fresh matching `binderd` as above.
