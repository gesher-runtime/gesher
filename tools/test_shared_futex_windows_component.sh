#!/usr/bin/env bash
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <LLVM-MinGW C++ compiler> <output.exe>" >&2
  exit 2
fi
cxx=$1
output=$2
root=$(cd "$(dirname "$0")/.." && pwd)
src="$root/compat/posix/src"

"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror -Wno-cast-function-type-strict \
  -Wno-unused-private-field -fno-exceptions -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
  -D_FILE_OFFSET_BITS=64 -I"$root/compat/posix/include" -I"$src" \
  -I"$root/compat/bionic" -I"$root/compat/elf/include" -I"$root/compat/arm64/include" \
  -municode -static -ffunction-sections -Wl,--gc-sections \
  "$root/tests/native/shared_futex_windows_component_test.cc" \
  "$root/compat/bionic/pthread_bionic.cc" \
  "$src/mman.cc" "$src/syscall.cc" "$src/shared_futex.cc" \
  "$src/record_lock_liveness.cc" "$src/internal.cc" \
  -lbcrypt -lpsapi -lsynchronization -lpthread -lkernel32 -lmincore -o "$output"
