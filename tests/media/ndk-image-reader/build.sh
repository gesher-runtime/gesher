#!/usr/bin/env bash
# Copyright 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1/media/ndk-image-reader" && cd "$1/media/ndk-image-reader" && pwd)
ndk=$(cd "$2" && pwd)
llvm=$ndk/toolchains/llvm/prebuilt
for host in linux-x86_64 darwin-x86_64 windows-x86_64; do
  if [ -d "$llvm/$host/bin" ]; then
    llvm=$llvm/$host
    break
  fi
done
for abi in x86_64 aarch64; do
  compiler="$llvm/bin/$abi-linux-android29-clang"
  if [ -f "$compiler.cmd" ]; then compiler=$compiler.cmd; fi
  common=(-std=c17 -O2 -Wall -Wextra -Werror "$here/image_reader_test.c" -lmediandk -landroid -lnativewindow -ldl -lEGL -lGLESv2)
  "$compiler" -DIMAGE_READER_STANDALONE -fPIE -pie "${common[@]}" -o "$out/image-reader-$abi"
  "$compiler" -fPIC -shared "${common[@]}" -o "$out/libimage-reader-$abi.so"
done
if [ "$#" -ge 3 ]; then
  classes=$(mktemp -d)
  trap 'rm -rf "$classes"' EXIT
  javac --release 8 -Xlint:-options -d "$classes" "$here/NdkImageReaderTest.java"
  "$3" --min-api 29 --output "$out" "$classes/gesher/NdkImageReaderTest.class"
  mv "$out/classes.dex" "$out/image-reader.dex"
fi
if [ "$#" -ge 4 ]; then
  android_jar=$4
  for abi in x86_64 aarch64; do
    compiler="$llvm/bin/$abi-linux-android29-clang"
    if [ -f "$compiler.cmd" ]; then compiler=$compiler.cmd; fi
    "$compiler" -std=c17 -O2 -Wall -Wextra -Werror -shared -fPIC "$here/yuv_reader_test.c" \
      -lmediandk -landroid -o "$out/libyuv-reader-$abi.so"
  done
  javac --release 8 -Xlint:-options -cp "$android_jar" -d "$classes" "$here/NdkYuvImageReaderTest.java"
  "$3" --min-api 29 --lib "$android_jar" --output "$out" "$classes/gesher/NdkYuvImageReaderTest.class"
  mv "$out/classes.dex" "$out/yuv-reader.dex"
  ffmpeg -v error -nostdin -y \
    -f lavfi -i 'color=red:size=64x48:rate=2:duration=0.5' \
    -f lavfi -i 'color=blue:size=64x48:rate=2:duration=0.5' \
    -filter_complex '[0:v][1:v]concat=n=2:v=1:a=0' \
    -c:v libx264 -pix_fmt yuv420p -profile:v baseline -g 1 -colorspace smpte170m \
    "$out/red-blue.mp4"
fi
