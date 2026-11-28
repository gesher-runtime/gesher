#!/usr/bin/env bash
# Copyright 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
# CI only: build the NDK AMediaCodec contract fixtures for x86_64 and arm64-v8a.
# Usage: build.sh <out> <android-ndk> <build-tools>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1/media/ndk-codec" && cd "$1/media/ndk-codec" && pwd)
ndk=$(cd "$2" && pwd)
build_tools=$(cd "$3" && pwd)
for llvm in "$ndk"/toolchains/llvm/prebuilt/{linux-x86_64,darwin-x86_64,windows-x86_64}; do
  if [ -d "$llvm/bin" ]; then break; fi
done
tool() {
  local compiler="$llvm/bin/$1"
  if [ -f "$compiler.cmd" ]; then compiler=$compiler.cmd; fi
  printf '%s\n' "$compiler"
}
for abi in x86_64 arm64-v8a; do
  case $abi in
    x86_64) triple=x86_64-linux-android ;;
    arm64-v8a) triple=aarch64-linux-android ;;
  esac
  # Public C++ contracts: exports, AVC round trip, Opus, ownership and callback lifecycle.
  mkdir -p "$out/$abi"
  "$(tool "${triple}29-clang++")" -std=c++20 -O2 -fstack-protector-strong -Wall -Wextra -Werror \
    -shared -fPIC -Wl,-soname,libndk_codec_test.so -o "$out/$abi/libndk_codec_test.so" \
    "$here/codec_test.cc" -lmediandk -ldl
  "$(tool "${triple}29-clang++")" -std=c++20 -O2 -fstack-protector-strong -Wall -Wextra -Werror \
    -fPIE -pie -DMEDIA_CODEC_EXECUTABLE -o "$out/$abi/ndk-codec-test" \
    "$here/codec_test.cc" -lmediandk -ldl
  cp "$llvm/sysroot/usr/lib/$triple/libc++_shared.so" "$out/$abi/"
  "$llvm/bin/llvm-readelf" -h "$out/$abi/ndk-codec-test" | head -20
  # Public C contracts: live encoder controls, parameter keys, rendered-frame
  # callbacks, ImageReader output surfaces and encoder input surfaces.
  arch=${triple%%-*}
  common=(-std=c17 -O2 -Wall -Wextra -Werror "$here/codec_test.c" -lmediandk -landroid -ldl)
  "$(tool "${triple}33-clang")" -DCODEC_STANDALONE -fPIE -pie "${common[@]}" -o "$out/ndk-codec-$arch"
  "$(tool "${triple}33-clang")" -DCODEC_PARAMETER_CONTROL_COMPONENT -fPIC -shared "${common[@]}" \
    -o "$out/libndk-codec-$arch.so"
  input=(-std=c17 -O2 -Wall -Wextra -Werror "$here/input_test.c" -lmediandk -lnativewindow)
  "$(tool "${triple}29-clang")" -DCODEC_STANDALONE -fPIE -pie "${input[@]}" -lEGL -lGLESv2 \
    -o "$out/ndk-codec-input-$arch"
  "$(tool "${triple}29-clang")" -DCODEC_INPUT_CPU_WINDOW -fPIC -shared "${input[@]}" \
    -o "$out/libndk-codec-input-$arch.so"
done
classes=$(mktemp -d)
trap 'rm -rf "$classes"' EXIT
mkdir -p "$classes/cxx" "$classes/c" "$out/dex"
javac --release 8 -Xlint:-options -d "$classes/cxx" "$here/MediaCodecNdkTest.java"
java -cp "$build_tools/lib/d8.jar" com.android.tools.r8.D8 --min-api 29 \
  --output "$out/dex" "$classes/cxx/gesher/MediaCodecNdkTest.class"
mv "$out/dex/classes.dex" "$out/ndk-codec-tests.dex"
javac --release 8 -Xlint:-options -d "$classes/c" "$here/NdkCodecTest.java"
java -cp "$build_tools/lib/d8.jar" com.android.tools.r8.D8 --min-api 29 \
  --output "$out/dex" "$classes/c/gesher/NdkCodecTest.class"
mv "$out/dex/classes.dex" "$out/ndk-codec.dex"
