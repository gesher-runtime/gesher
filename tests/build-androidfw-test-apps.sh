#!/usr/bin/env bash
# The test apps libandroidfw_tests reads next to its executable, besides
# tests/data: FrameworkResourcesSparseTestApp.apk and
# FrameworkResourcesNotSparseTestApp.apk. Mirrors
# frameworks/base/libs/androidfw/tests/data/sparse/Android.bp: two
# android_test_helper_app modules over that directory (sdk current, min_sdk
# 32), the first linked with --enable-sparse-encoding. framework-res.apk
# stands in for the SDK's android.jar as the include.
#
#   tests/build-androidfw-test-apps.sh <aapt2> <framework-res.apk> <out-dir>
set -euo pipefail

aapt2=$1
framework_res=$2
out=$(mkdir -p "$3" && cd "$3" && pwd)
root=$(cd "$(dirname "$0")/.." && pwd)
aosp=${AOSP_DIR:-$root/third_party/aosp}
src=$aosp/frameworks/base/libs/androidfw/tests/data/sparse
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$aapt2" compile --dir "$src/res" -o "$work/compiled.zip"
link() {  # link <app name> [aapt2 flags...]
  local name=$1
  shift
  "$aapt2" link --manifest "$src/AndroidManifest.xml" -I "$framework_res" \
    --min-sdk-version 32 --target-sdk-version 36 "$@" \
    -o "$out/$name.apk" "$work/compiled.zip"
  echo "$out/$name.apk"
}
link FrameworkResourcesSparseTestApp --enable-sparse-encoding
link FrameworkResourcesNotSparseTestApp
