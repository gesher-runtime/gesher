#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [[ $# -ne 3 ]]; then
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
"$tools/aapt2" link -I "$platform" --manifest "$here/AndroidManifest.xml" -o "$work/raw.apk"
javac -source 17 -target 17 -Xlint:-options -XDstringConcat=inline -cp "$platform" \
    -d "$work/classes" "$here/PersistenceActivity.java"
jar --create --file "$work/classes.jar" --no-manifest -C "$work/classes" .
if [[ -n ${R8_JAR:-} ]]; then
    dex=(java -cp "$R8_JAR" com.android.tools.r8.D8)
else
    dex=("$tools/d8")
fi
"${dex[@]}" --release --min-api 36 --lib "$platform" --output "$work/dex" "$work/classes.jar"
jar uf "$work/raw.apk" -C "$work/dex" classes.dex
"$tools/zipalign" -f 4 "$work/raw.apk" "$work/aligned.apk"
if [[ ! -e $key ]]; then
    keytool -genkeypair -keystore "$key" -storetype PKCS12 -storepass android \
        -alias storage-test -keyalg RSA -keysize 2048 -validity 3650 \
        -dname 'CN=Gesher disposable persistence fixture'
fi
"$tools/apksigner" sign --ks "$key" --ks-key-alias storage-test \
    --ks-pass pass:android --out "$out" "$work/aligned.apk"
"$tools/apksigner" verify --print-certs "$out"
