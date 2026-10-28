#!/usr/bin/env bash
# CI only: the same real muxer contract as an NDK JNI library and ELF executable.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1/media/ndk-muxer" && cd "$1/media/ndk-muxer" && pwd)
ndk=$(cd "$2" && pwd)
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
for abi in x86_64 arm64-v8a; do
  case $abi in
    x86_64) triple=x86_64-linux-android ;;
    arm64-v8a) triple=aarch64-linux-android ;;
  esac
  mkdir -p "$out/$abi"
  "$llvm/bin/${triple}29-clang++" -std=c++20 -O2 -fstack-protector-strong -Wall -Wextra -Werror \
    -shared -fPIC -Wl,-soname,libmediamuxer_test.so -o "$out/$abi/libmediamuxer_test.so" \
    "$here/muxer_test.cc" -lmediandk -ldl
  "$llvm/bin/${triple}29-clang++" -std=c++20 -O2 -fstack-protector-strong -Wall -Wextra -Werror \
    -fPIE -pie -DMEDIA_MUXER_EXECUTABLE -o "$out/$abi/ndk-muxer-test" \
    "$here/muxer_test.cc" -lmediandk -ldl
  cp "$llvm/sysroot/usr/lib/$triple/libc++_shared.so" "$out/$abi/"
  "$llvm/bin/llvm-readelf" -h "$out/$abi/ndk-muxer-test" | head -20
done
