#!/usr/bin/env bash
# Copyright 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1/media/ndk-codec" && cd "$1/media/ndk-codec" && pwd)
ndk=$(cd "$2" && pwd)
for llvm in "$ndk"/toolchains/llvm/prebuilt/{linux-x86_64,darwin-x86_64,windows-x86_64}; do
  if [ -d "$llvm/bin" ]; then break; fi
done
for abi in x86_64 aarch64; do
  compiler="$llvm/bin/$abi-linux-android33-clang"
  if [ -f "$compiler.cmd" ]; then compiler=$compiler.cmd; fi
  common=(-std=c17 -O2 -Wall -Wextra -Werror "$here/codec_test.c" -lmediandk -landroid -ldl)
  "$compiler" -DCODEC_STANDALONE -fPIE -pie "${common[@]}" -o "$out/ndk-codec-$abi"
  "$compiler" -DCODEC_PARAMETER_CONTROL_COMPONENT -fPIC -shared "${common[@]}" -o "$out/libndk-codec-$abi.so"
  compiler="$llvm/bin/$abi-linux-android29-clang"
  if [ -f "$compiler.cmd" ]; then compiler=$compiler.cmd; fi
  input=(-std=c17 -O2 -Wall -Wextra -Werror "$here/input_test.c" -lmediandk -lnativewindow)
  "$compiler" -DCODEC_STANDALONE -fPIE -pie "${input[@]}" -lEGL -lGLESv2 -o "$out/ndk-codec-input-$abi"
  "$compiler" -DCODEC_INPUT_CPU_WINDOW -fPIC -shared "${input[@]}" -o "$out/libndk-codec-input-$abi.so"
done
if [ "$#" -ge 3 ]; then
  classes=$(mktemp -d)
  trap 'rm -rf "$classes"' EXIT
  javac --release 8 -Xlint:-options -d "$classes" "$here/NdkCodecTest.java"
  "$3" --min-api 29 --output "$out" "$classes/gesher/NdkCodecTest.class"
  mv "$out/classes.dex" "$out/ndk-codec.dex"
fi
