#!/usr/bin/env bash
# Builds popular NDK libraries and their own test programs for one ABI with
# the NDK, as Android ELF executables (PIEs) that gesher-exec runs against the
# bionic shim (tests/windows/tracks/44-native2.ps1, ADR 0057):
#
#   tests/native2/build-third-party.sh <ndk dir> <x86_64|arm64-v8a> <out dir>
#
#   zlib 1.3.1        test/example and minigzip, against the platform's libz.so
#   libpng 1.6.43     pngtest (and pngtest.png), against libz.so
#   libjpeg-turbo 3.0.4  tjunittest (TurboJPEG round trips, SIMD on)
#   SQLite 3.46.1     the sqlite3 shell (amalgamation)
#   OpenSSL 3.3.2     the openssl command (static libcrypto/libssl)
#
# Sources are pinned release archives; their SHA-256 is printed. Runs on the
# Linux runner (.github/workflows/native2.yml).
set -euo pipefail

ndk=$(cd "$1" && pwd)
abi=$2
out=$(mkdir -p "$3" && cd "$3" && pwd)
case $abi in
  x86_64) triple=x86_64-linux-android; ossl=android-x86_64 ;;
  arm64-v8a) triple=aarch64-linux-android; ossl=android-arm64 ;;
  *) echo "unknown ABI $abi" >&2; exit 2 ;;
esac
api=29
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
cc="$llvm/bin/${triple}${api}-clang"
jobs=$(nproc)
src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT

fetch() {  # url file
  curl -fsSL --retry 3 -o "$src/$2" "$1"
  echo "$(sha256sum "$src/$2" | cut -d' ' -f1)  $2 ($1)"
}

# --- zlib ---------------------------------------------------------------------
fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz zlib.tar.gz
tar -xzf "$src/zlib.tar.gz" -C "$src"
# The NDK's <zlib.h> (the platform's libz.so), not the archive's.
"$cc" -O2 -Wall -o "$out/zlib_example" "$src/zlib-1.3.1/test/example.c" -lz
"$cc" -O2 -Wall -o "$out/zlib_minigzip" "$src/zlib-1.3.1/test/minigzip.c" -lz

# --- libpng -------------------------------------------------------------------
fetch https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.43.tar.gz libpng.tar.gz
tar -xzf "$src/libpng.tar.gz" -C "$src"
png=$src/libpng-1.6.43
cp "$png/scripts/pnglibconf.h.prebuilt" "$png/pnglibconf.h"
"$cc" -O2 -DPNG_ARM_NEON_OPT=0 -DPNG_INTEL_SSE_OPT=0 -I"$png" -o "$out/pngtest" \
  "$png"/png.c "$png"/pngerror.c "$png"/pngget.c "$png"/pngmem.c "$png"/pngpread.c \
  "$png"/pngread.c "$png"/pngrio.c "$png"/pngrtran.c "$png"/pngrutil.c "$png"/pngset.c \
  "$png"/pngtrans.c "$png"/pngwio.c "$png"/pngwrite.c "$png"/pngwtran.c "$png"/pngwutil.c \
  "$png"/pngtest.c -lz -lm
cp "$png/pngtest.png" "$out/pngtest.png"

# --- libjpeg-turbo ------------------------------------------------------------------
fetch https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.0.4/libjpeg-turbo-3.0.4.tar.gz jpeg.tar.gz
tar -xzf "$src/jpeg.tar.gz" -C "$src"
cmake -S "$src/libjpeg-turbo-3.0.4" -B "$src/jpeg-build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$abi" \
  -DANDROID_PLATFORM=android-$api -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=ON >/dev/null
cmake --build "$src/jpeg-build" --target tjunittest -j "$jobs" >/dev/null
cp "$src/jpeg-build/tjunittest" "$out/tjunittest"

# --- SQLite -------------------------------------------------------------------------
fetch https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip sqlite.zip
(cd "$src" && unzip -q sqlite.zip)
sq=$src/sqlite-amalgamation-3460100
"$cc" -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_MATH_FUNCTIONS \
  -o "$out/sqlite3" "$sq/shell.c" "$sq/sqlite3.c" -ldl -lm

# --- OpenSSL ------------------------------------------------------------------------
fetch https://github.com/openssl/openssl/releases/download/openssl-3.3.2/openssl-3.3.2.tar.gz openssl.tar.gz
tar -xzf "$src/openssl.tar.gz" -C "$src"
(
  cd "$src/openssl-3.3.2"
  export ANDROID_NDK_ROOT=$ndk PATH=$llvm/bin:$PATH
  ./Configure "$ossl" -D__ANDROID_API__=$api no-shared no-tests no-docs no-ui-console >/dev/null
  make -j "$jobs" build_programs >/dev/null
)
cp "$src/openssl-3.3.2/apps/openssl" "$out/openssl"

"$llvm/bin/llvm-strip" "$out"/zlib_example "$out"/zlib_minigzip "$out"/pngtest "$out"/tjunittest \
  "$out"/sqlite3 "$out"/openssl
ls -la "$out"
