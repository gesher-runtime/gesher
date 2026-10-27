#!/usr/bin/env bash
# CI only: actual NDK JNI callers for both guest ABIs.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1/media/opensl-contracts" && cd "$1/media/opensl-contracts" && pwd)
ndk=$(cd "$2" && pwd)
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
for abi in x86_64 arm64-v8a; do
  case $abi in
    x86_64) triple=x86_64-linux-android ;;
    arm64-v8a) triple=aarch64-linux-android ;;
  esac
  mkdir -p "$out/$abi"
  "$llvm/bin/${triple}29-clang++" -std=c++20 -O2 -fstack-protector-strong -Wall -Wextra -Werror \
    -shared -fPIC -Wl,-soname,libopensl_contracts.so -o "$out/$abi/libopensl_contracts.so" \
    "$here/opensl_test.cc" -lOpenSLES -ldl
  cp "$llvm/sysroot/usr/lib/$triple/libc++_shared.so" "$out/$abi/"
  "$llvm/bin/llvm-readelf" -h "$out/$abi/libopensl_contracts.so" | head -20
done
