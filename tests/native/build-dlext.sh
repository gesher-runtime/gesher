#!/usr/bin/env bash
# Builds tests/native/dlext_test.c and the libraries it loads with
# android_dlopen_ext (tests/native/dlext_target.c) into <outdir>:
#   build-dlext.sh <ndk clang for the ABI, API >= 29> <outdir>
# native-libs.yml (x86_64) and arm64.yml (arm64-v8a) call it.
set -euo pipefail
cc=$1
out=$2
src=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
flags="-shared -fPIC -O2 -Wall -Wextra -Werror"
"$cc" $flags -Wl,-soname,libdlext_test.so -o "$out/dlext_test.so" "$src/dlext_test.c" -ldl
# One soname per case that loads a library (a loaded one is returned as is).
for n in a b c d e both huge hole chrome churn split; do
  "$cc" $flags -Wl,-soname,libdlext_$n.so -o "$out/libdlext_$n.so" "$src/dlext_target.c"
done
# A first PT_LOAD at p_vaddr 0x200000, and a 4 MiB gap between segments
# (loaded into a reservation, and loaded anywhere).
"$cc" $flags -Wl,--image-base=0x200000 -Wl,-soname,libdlext_vaddr.so \
  -o "$out/libdlext_vaddr.so" "$src/dlext_target.c"
for n in gap gap2; do
  "$cc" $flags -Wl,--section-start=.data=0x400000 -Wl,-soname,libdlext_$n.so \
    -o "$out/libdlext_$n.so" "$src/dlext_target.c"
done
# Each with a symbol of its own (GESHER_DLEXT_MARKER).
for m in close bind default unmap; do
  "$cc" $flags -DGESHER_DLEXT_MARKER=gesher_dlext_${m}_marker -Wl,-soname,libdlext_$m.so \
    -o "$out/libdlext_$m.so" "$src/dlext_target.c"
done
# Binds gesher_dlext_bind_marker through the global scope (no DT_NEEDED).
"$cc" $flags -DGESHER_DLEXT_USER -Wl,-soname,libdlext_user.so \
  -o "$out/libdlext_user.so" "$src/dlext_target.c"
# A segment of many pages (64 KiB .bss).
"$cc" $flags -DGESHER_DLEXT_BSS -Wl,-soname,libdlext_cross.so \
  -o "$out/libdlext_cross.so" "$src/dlext_target.c"
# Registers a thread_local destructor with its __dso_handle.
"$cc" $flags -DGESHER_DLEXT_THREAD_DTOR -Wl,-soname,libdlext_tdtor.so \
  -o "$out/libdlext_tdtor.so" "$src/dlext_target.c"
# Its load fails at relocation (an undefined thread-local), after mapping.
"$cc" $flags -DGESHER_DLEXT_TLS_FAIL -Wl,-soname,libdlext_tlsfail.so \
  -o "$out/libdlext_tlsfail.so" "$src/dlext_target.c"
