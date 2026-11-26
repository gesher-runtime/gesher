#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build a signed fixture with SDK build-tools 36.0.0 and JDK 17+.
set -euo pipefail

if [[ $# != 3 ]]; then
    echo "usage: $0 SDK_ROOT OUTPUT.apk TEST_KEYSTORE.p12" >&2
    exit 2
fi
sdk=$(cd "$1" && pwd)
out=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
key=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
here=$(cd "$(dirname "$0")" && pwd)
tools=$sdk/build-tools/36.0.0
platform=$sdk/platforms/android-36/android.jar
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/classes" "$work/dex"

"$tools/aapt2" link -I "$platform" --manifest "$here/AndroidManifest.xml" \
    --min-sdk-version 36 --target-sdk-version 36 -o "$work/unaligned.apk"
javac --release 17 -classpath "$platform" -d "$work/classes" "$here/StorageActivity.java"
"$tools/d8" --min-api 36 --lib "$platform" --output "$work/dex" \
    "$work/classes/org/gesher/contracts/storage/StorageActivity.class"
(cd "$work/dex" && jar -u -M -f "$work/unaligned.apk" classes.dex)
"$tools/zipalign" -f 4 "$work/unaligned.apk" "$work/aligned.apk"
# Use a dedicated local fixture-only key. Reuse it for repeatable reinstall tests.
if [[ ! -e $key ]]; then
    keytool -genkeypair -keystore "$key" -storetype PKCS12 -storepass android \
        -alias storage-test -keyalg RSA -keysize 2048 -validity 3650 \
        -dname 'CN=Gesher storage test fixture'
fi
"$tools/apksigner" sign --ks "$key" --ks-key-alias storage-test \
    --ks-pass pass:android --out "$out" "$work/aligned.apk"
"$tools/apksigner" verify --print-certs "$out"
