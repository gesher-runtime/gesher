#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 SDK_ROOT OUT_DIR" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
sdk=$(cd "$1" && pwd)
out=$(mkdir -p "$2" && cd "$2" && pwd)
tools=$sdk/build-tools/36.0.0
platform=$sdk/platforms/android-37.0/android.jar
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
mkdir "$scratch/classes" "$scratch/dex"
key=$out/fixture.p12
if [[ ! -f $key ]]; then
    keytool -genkeypair -alias lan-fixture -storetype PKCS12 -keyalg RSA -keysize 2048 \
        -keystore "$key" -storepass android -keypass android -dname 'CN=Gesher LAN test fixture' -validity 365
fi
javac --release 8 -encoding UTF-8 -classpath "$platform" -d "$scratch/classes" "$here/LanProbe.java"
jar cf "$scratch/classes.jar" -C "$scratch/classes" .
"$tools/d8" --min-api 36 --lib "$platform" --output "$scratch/dex" "$scratch/classes.jar"
for target in 36 37; do
    permission=
    if [[ $target -eq 37 ]]; then
        permission='<uses-permission android:name="android.permission.ACCESS_LOCAL_NETWORK"/>'
    fi
    cat > "$scratch/AndroidManifest.xml" <<XML
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.gesher.contracts.lan.t$target">
  <uses-permission android:name="android.permission.INTERNET"/>
  <uses-permission android:name="android.permission.NEARBY_WIFI_DEVICES"/>
  $permission
  <application android:label="Gesher LAN probe $target" android:debuggable="true">
    <activity android:name="org.gesher.contracts.lan.LanProbe" android:exported="true"/>
  </application>
</manifest>
XML
    "$tools/aapt2" link -I "$platform" --manifest "$scratch/AndroidManifest.xml" \
        --min-sdk-version 36 --target-sdk-version "$target" -o "$scratch/unsigned.apk"
    jar ufM "$scratch/unsigned.apk" -C "$scratch/dex" classes.dex
    "$tools/zipalign" -f 4 "$scratch/unsigned.apk" "$scratch/aligned.apk"
    "$tools/apksigner" sign --ks "$key" --ks-key-alias lan-fixture --ks-pass pass:android \
        --out "$out/target-$target.apk" "$scratch/aligned.apk"
    "$tools/apksigner" verify "$out/target-$target.apk"
done
python3 - "$out" <<'PY'
import hashlib
from pathlib import Path
import sys

for apk in sorted(Path(sys.argv[1]).glob("target-*.apk")):
    print(hashlib.sha256(apk.read_bytes()).hexdigest(), apk.name)
PY
