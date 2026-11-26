#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Make one signed APK per guest ABI so Windows exercises both.
set -euo pipefail
if [[ $# -ne 4 ]]; then
    echo "usage: $0 SDK_ROOT NDK_ROOT OUTPUT_DIRECTORY TEST_KEYSTORE.p12" >&2
    exit 2
fi
sdk=$(cd "$1" && pwd)
ndk=$(cd "$2" && pwd)
out=$(mkdir -p "$3" && cd "$3" && pwd)
key=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")
here=$(cd "$(dirname "$0")" && pwd)
tools=$sdk/build-tools/36.0.0
platform=$sdk/platforms/android-36/android.jar
hosts=("$ndk"/toolchains/llvm/prebuilt/*)
compiler=${hosts[0]}/bin/clang
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/classes" "$work/dex"
javac -source 8 -target 8 -Xlint:-options -bootclasspath "$platform" -d "$work/classes" "$here/"*.java
"$tools/d8" --min-api 24 --lib "$platform" --output "$work/dex" \
    "$work/classes/org/gesher/contracts/camerandk/"*.class
if [[ ! -e $key ]]; then
    keytool -genkeypair -keystore "$key" -storetype PKCS12 -storepass android \
        -alias camera-ndk-test -keyalg RSA -keysize 2048 -validity 3650 \
        -dname 'CN=Gesher Camera NDK enumeration test fixture'
fi
for abi in arm64-v8a x86_64; do
    native=$work/$abi
    mkdir -p "$native/lib/$abi"
    target=x86_64-linux-android24
    if [[ $abi == arm64-v8a ]]; then target=aarch64-linux-android24; fi
    "$compiler" --target="$target" -shared -fPIC -std=c17 -Wall -Wextra -Werror \
        -Wl,-z,max-page-size=16384 -o "$native/lib/$abi/libcamera-enumeration.so" \
        "$here/camera_enumeration.c" -lcamera2ndk -ldl
    "$tools/aapt2" link -I "$platform" --manifest "$here/AndroidManifest.xml" \
        --min-sdk-version 24 --target-sdk-version 36 -o "$native/unaligned.apk"
    (cd "$work/dex" && jar -u -M -f "$native/unaligned.apk" classes.dex)
    (cd "$native" && jar -u -M -f unaligned.apk lib)
    "$tools/zipalign" -f 4 "$native/unaligned.apk" "$native/aligned.apk"
    "$tools/apksigner" sign --ks "$key" --ks-key-alias camera-ndk-test --ks-pass pass:android \
        --out "$out/camera-ndk-enumeration-$abi.apk" "$native/aligned.apk"
    "$tools/apksigner" verify --print-certs "$out/camera-ndk-enumeration-$abi.apk"
done
