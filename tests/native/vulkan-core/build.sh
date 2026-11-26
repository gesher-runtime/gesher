#!/usr/bin/env bash
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [[ $# != 3 ]]; then
    echo "usage: $0 NDK_TOOLCHAIN_BIN R8_JAR OUTPUT_DIR" >&2
    exit 2
fi
bin=$1
r8=$2
out=$3
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"

for abi in aarch64 x86_64; do
    cc=$bin/$abi-linux-android35-clang
    "$cc" -std=c11 -O2 -Wall -Wextra -Werror "$here/core_discovery.c" \
        -lvulkan -ldl -o "$out/vulkan-core-$abi"
    "$cc" -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC -DGESHER_VULKAN_JNI \
        "$here/core_discovery.c" -Wl,-z,defs -lvulkan -ldl -o "$out/libvulkan-core-$abi.so"
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
javac --release 8 -d "$work" "$here/VulkanCoreTest.java"
java -cp "$r8" com.android.tools.r8.D8 --min-api 35 --output "$work" \
    "$work/gesher/VulkanCoreTest.class"
mv "$work/classes.dex" "$out/vulkan-core.dex"
