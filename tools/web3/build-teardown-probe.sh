#!/usr/bin/env bash
# CI only: the existing Gesher WCT/native-stack implementation, no AOSP build.
set -euo pipefail
toolchain=${1:?llvm-mingw directory required}
output=${2:-dist-web3-observer}
mkdir -p "$output"
"$toolchain/bin/x86_64-w64-mingw32-clang++" --version
"$toolchain/bin/x86_64-w64-mingw32-clang++" \
  -std=gnu++20 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Wall -Wextra -Werror \
  -mno-ms-bitfields -fno-exceptions -fsized-deallocation -static \
  -Icompat/input probes/web3/process_waits.cc compat/input/thread_watch.cc \
  -o "$output/web3_process_waits.exe" -lole32 -luuid
