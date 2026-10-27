#!/usr/bin/env bash
# CI-only independent oracle. QEMU's ARM/SoftFloat code is unmodified.
set -euo pipefail
output=$1
reference_dir=$(cd "$(dirname "$0")" && pwd)
pin=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["commit"])' "$reference_dir/qemu.lock.json")
mkdir -p "$output"
output=$(cd "$output" && pwd)
work=$(mktemp -d "${RUNNER_TEMP:?}/gesher-qemu.XXXXXX")
git init -q "$work/source"
git -C "$work/source" fetch -q --depth 1 https://github.com/qemu/qemu.git "$pin"
git -C "$work/source" checkout -q --detach FETCH_HEAD
test "$(git -C "$work/source" rev-parse HEAD)" = "$pin"
mkdir "$work/build"
cd "$work/build"
"$work/source/configure" --target-list=aarch64-linux-user --disable-docs --disable-werror
ninja -j 4 qemu-aarch64
cp qemu-aarch64 "$output/"
cp "$work/source/COPYING" "$output/QEMU-COPYING"
printf '%s\n' "$pin" > "$output/source-commit.txt"
cd "$output"
sha256sum qemu-aarch64 > binary.sha256
./qemu-aarch64 --version
printf 'PINNED_QEMU source %s\n' "$pin"
cat binary.sha256
