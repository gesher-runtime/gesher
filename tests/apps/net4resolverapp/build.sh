#!/usr/bin/env bash
# Package the exact NDK library already built for dalvikvm, with/without INTERNET.
set -euo pipefail
app=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
abi=$2
shift 2
android_jar= tools= r8=
while [[ $# -gt 0 ]]; do
  case $1 in
    --android-jar) android_jar=$2; shift 2 ;;
    --build-tools) tools=$2; shift 2 ;;
    --r8) r8=$2; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
[[ $abi == x86_64 || $abi == arm64-v8a ]]
[[ -n $android_jar && -n $tools && -n $r8 ]]
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/classes" "$work/dex" "$work/apk/lib/$abi"
cp "$out/$abi/libnet4resolver.so" "$out/$abi/libc++_shared.so" "$work/apk/lib/$abi/"
javac --release 11 -encoding UTF-8 -cp "$android_jar" -d "$work/classes" $(find "$app/java" -name '*.java')
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 29 --lib "$android_jar" \
  --output "$work/dex" $(find "$work/classes" -name '*.class')
"$tools/aapt2" compile --dir "$app/res" -o "$work/res.zip"
for kind in allow deny; do
  cp "$app/AndroidManifest.xml" "$work/AndroidManifest.xml"
  name=net4resolverapp
  if [[ $kind == deny ]]; then
    name=net4resolverdenied
    python3 - "$work/AndroidManifest.xml" <<'PY'
from pathlib import Path
import sys
import xml.etree.ElementTree as ET
path = Path(sys.argv[1])
ET.register_namespace('android', 'http://schemas.android.com/apk/res/android')
tree = ET.parse(path)
root = tree.getroot()
root.set('package', 'com.gesher.net4resolver.denied')
permissions = root.findall('uses-permission')
assert len(permissions) == 1
root.remove(permissions[0])
tree.write(path, encoding='utf-8', xml_declaration=True)
PY
  fi
  "$tools/aapt2" link -o "$work/$kind.apk" -I "$android_jar" \
    --manifest "$work/AndroidManifest.xml" --min-sdk-version 29 --target-sdk-version 36 "$work/res.zip"
  (cd "$work/dex" && zip -q "$work/$kind.apk" classes.dex)
  (cd "$work/apk" && zip -q -0 "$work/$kind.apk" lib/$abi/*.so)
  "$tools/zipalign" -f -p 4 "$work/$kind.apk" "$out/$name-$abi.apk"
  echo "$name-$abi.apk: exact NDK resolver library packaged; permission=$kind"
done
