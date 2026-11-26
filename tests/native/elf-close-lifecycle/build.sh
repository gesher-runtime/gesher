#!/usr/bin/env bash
# Build a public dlopen/dlclose probe using Linux or Android NDK clang.
# Usage: bash build.sh <C compiler> <output directory>
set -euo pipefail
cc=$1
out=$2
src=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
shared=(-O2 -Wall -Wextra -Werror -shared -fPIC -nostdlib -Wl,--hash-style=both)
"$cc" "${shared[@]}" -Wl,-soname,libn06_trace.so -o "$out/libn06_trace.so" "$src/trace.c"
"$cc" "${shared[@]}" -Wl,-soname,libn06_dep.so '-Wl,-rpath,$ORIGIN' \
  -DGESHER_N06_DEPENDENCY -o "$out/libn06_dep.so" "$src/library.c" -L"$out" -ln06_trace
"$cc" "${shared[@]}" -Wl,-soname,libn06_root.so '-Wl,-rpath,$ORIGIN' \
  -o "$out/libn06_root.so" "$src/library.c" -L"$out" -ln06_dep -ln06_trace
"$cc" -O2 -Wall -Wextra -Werror -o "$out/elf-close-lifecycle" "$src/oracle.c" -ldl
