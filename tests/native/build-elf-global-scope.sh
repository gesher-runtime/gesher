#!/usr/bin/env bash
# Build the N02 relocation scope fixtures and public Android NDK executable.
# Usage: bash build-elf-global-scope.sh <NDK clang> <outdir>
# Run elf-global-scope <fixture-directory> from an Android debug shell with the
# generated executable and shared objects in that directory. A Windows build
# can run the same guest program with:
#   gesher-exec.exe --elf <path-to-elf-global-scope> elf-global-scope <fixture-directory>
set -euo pipefail
cc=$1
out=$2
src=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
common=(-shared -fPIC -O2 -Wall -Wextra -Werror -nostdlib -Wl,--hash-style=both)
value="$src/elf_global_scope_value.c"
reference="$src/elf_global_scope_reference.c"
build() {
  local name=$1 symbol=$2 result=$3
  shift 3
  "$cc" "${common[@]}" "-DSCOPE_SYMBOL=gesher_scope_$symbol" "-DSCOPE_VALUE=$result" \
    "-Wl,-soname,scope_$name.so" '-Wl,-rpath,$ORIGIN' \
    -o "$out/scope_$name.so" -L"$out" -Wl,--no-as-needed "$@"
}
build ordinary resident 11 "$value"
build control resident 33 "$value" "$reference"
build global resident 22 "$value" -Wl,-z,global
build resident resident 34 "$value" "$reference"
build deep pending 51 "$value" -Wl,-z,global
build intermediate intermediate 35 "$value" -l:scope_deep.so
build direct pending 52 "$value" -Wl,-z,global
build pending pending 31 "$value" "$reference" -l:scope_intermediate.so -l:scope_direct.so
build later pending 37 "$value" "$reference"
"$cc" -O2 -Wall -Wextra -Werror -o "$out/elf-global-scope" \
  "$src/elf_global_scope_oracle.c" -ldl
