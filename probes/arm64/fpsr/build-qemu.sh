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

# Stage the exact wrap revisions before Meson configures. Meson still reads
# QEMU's original wrap files and applies the original package overlays.
fetch_wrap() {
    local name=$1 wrap url revision mirror
    wrap="$work/source/subprojects/$name.wrap"
    url=$(sed -n 's/^url = //p' "$wrap")
    revision=$(sed -n 's/^revision = //p' "$wrap")
    test "$url" = "https://gitlab.com/qemu-project/$name.git"
    [[ $revision =~ ^[0-9a-f]{40}$ ]]
    mirror="$work/$name-primary.git"
    git init -q --bare "$mirror"
    if ! timeout --kill-after=5s 35s env GIT_TERMINAL_PROMPT=0 \
            git -C "$mirror" fetch -q --depth 1 "$url" "$revision"; then
        printf 'QEMU wrap %s unavailable on GitLab, fetching the same revision from the QEMU mirror\n' "$name" >&2
        mirror="$work/$name-alternate.git"
        git init -q --bare "$mirror"
        timeout --kill-after=5s 60s env GIT_TERMINAL_PROMPT=0 \
            git -C "$mirror" fetch -q --depth 1 "https://github.com/qemu/$name.git" "$revision"
    fi
    test "$(git -C "$mirror" rev-parse 'FETCH_HEAD^{commit}')" = "$revision"
    git -C "$mirror" update-ref refs/heads/verified "$revision"
    git config --file "$work/verified-wraps.gitconfig" --add "url.$mirror.insteadOf" "$url"
    printf 'PINNED_QEMU_WRAP %s %s\n' "$name" "$revision"
}

fetch_wrap berkeley-softfloat-3
fetch_wrap berkeley-testfloat-3
mkdir "$work/build"
cd "$work/build"
GIT_CONFIG_GLOBAL="$work/verified-wraps.gitconfig" \
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
