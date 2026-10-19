#!/usr/bin/env bash
# Compile the Java test programs to DEX with d8 and package an APK-style zip.
#
#   tests/build-test-apps.sh <out-dir>
#
# Needs a JDK (javac, java). d8 comes from Google's Maven repository.
set -euo pipefail

R8_VERSION=9.5.26
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

r8=${R8_JAR:-$work/r8.jar}
if [[ ! -f $r8 ]]; then
  curl -fsSL -o "$r8" \
    "https://dl.google.com/android/maven2/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar"
fi

javac --release 11 -d "$work/classes" "$root"/tests/dex/*.java
mkdir -p "$work/dex"
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 26 \
  --output "$work/dex" $(find "$work/classes" -name '*.class')

cp "$work/dex/classes.dex" "$out/hello.dex"
python3 - "$work/dex/classes.dex" "$out/hello.apk" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED) as z:
    z.write(sys.argv[1], "classes.dex")
PY
echo "test apps written to $out"
