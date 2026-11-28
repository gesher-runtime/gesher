# Anonymous pipe write readiness after the last reader closes

This standalone [N31](https://github.com/gesher-runtime/gesher/issues/56) probe
checks `poll` on an empty and a full nonblocking anonymous pipe. Closing one
read descriptor leaves its alias live. After closing the alias, both cases
report `POLLERR` and reject writes with `EPIPE`. Android 16 and Ubuntu also
continue to report available write room for the formerly empty pipe. The
formerly full pipe has no room and reports only `POLLERR`.

## Run the probe

On Linux:

```sh
c++ -std=c++20 -Wall -Wextra -Werror tests/native/pipe_write_readiness_probe.cc -o /tmp/pipe-write-readiness
/tmp/pipe-write-readiness
```

For Android, compile the same source with `aarch64-linux-android28-clang++`
and `-static-libstdc++`. Push the executable to `/data/local/tmp` and run it
with `adb shell`.

On a Windows cross-build host, configure the pipe component and select the
standalone target. `AOSP_DIR` can be populated with `prepare-inputs.py` in
this directory. For ARM64, use `toolchain-llvm-mingw-arm64.cmake` and a fresh
build directory.

```sh
cmake -S tools/pipe-component -B out-pipe \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-llvm-mingw.cmake" \
  -DLLVM_MINGW=/path/to/pinned/llvm-mingw -DAOSP_DIR=/path/to/pinned/aosp
cmake --build out-pipe --target pipe_write_readiness_probe
```

Run `pipe_write_readiness_probe.exe` in Windows. Exit 1 denotes a contract
mismatch. The target is outside the default build and normal pass gates.

## Observed behavior

| Target | Checks passed |
| --- | ---: |
| Android 16 ARM64, `BE2A.250530.026.D1` | 21/21 |
| Ubuntu 24.04 ARM64 | 21/21 |
| Windows 11 x64 component under emulation | 19/21 |
| Windows 11 ARM64 component | 19/21 |

Both Windows component runs linked the production POSIX library. The empty
pipe lost its write-room flags after its last reader closed. The full pipe
correctly kept only `POLLERR`. The native metadata samples exposed the reason
both cases need more bookkeeping: once the last reader closes, NPFS moves to
state 4 and reports all 65,536 bytes of write quota for both pipes, including
a pipe whose quota was zero just before closure. A failed write leaves the
same samples. Using the disconnected quota would report write room for the
formerly full pipe as well.

A faithful fix needs occupancy evidence spanning all aliases and processes
using the pipe, including readers that leave abruptly. The anonymous pipe
post-close write-room item remains open.
