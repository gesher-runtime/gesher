#!/usr/bin/env bash
# CI only: exact common actor body in both native ELF and ART/APK libraries.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
out= ndk= android_jar= build_tools= r8=
while [[ $# -gt 0 ]]; do
  case $1 in
    --out) out=$2; shift 2 ;;
    --ndk) ndk=$2; shift 2 ;;
    --android-jar) android_jar=$2; shift 2 ;;
    --build-tools) build_tools=$2; shift 2 ;;
    --r8) r8=$2; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
[[ -n $out && -n $ndk && -n $android_jar && -n $build_tools && -n $r8 ]]
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/classes" "$work/dex"
javac --release 11 -encoding UTF-8 -cp "$android_jar" -d "$work/classes" \
  "$source_dir/java/gesher/BinderSgActor.java" \
  "$source_dir/app/java/com/gesher/bindersg/SgActivity.java"
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 29 --lib "$android_jar" \
  --output "$work/dex" $(find "$work/classes" -name '*.class')
cp "$work/dex/classes.dex" "$out/binder-sg.dex"
llvm="$ndk/toolchains/llvm/prebuilt/linux-x86_64"
for abi in x86_64 arm64-v8a; do
  case $abi in x86_64) triple=x86_64-linux-android ;; *) triple=aarch64-linux-android ;; esac
  mkdir -p "$out/$abi" "$work/$abi/lib/$abi"
  "$llvm/bin/${triple}29-clang++" -std=c++20 -O2 -Wall -Wextra -Werror -fstack-protector-strong \
    -o "$out/$abi/binder-sg-actor" "$source_dir/sg-actor.cc"
  "$llvm/bin/${triple}29-clang++" -std=c++20 -O2 -Wall -Wextra -Werror -fstack-protector-strong \
    -shared -fPIC -Wl,-soname,libbindersg.so -o "$out/$abi/libbindersg.so" "$source_dir/sg-jni.cc"
  cp "$llvm/sysroot/usr/lib/$triple/libc++_shared.so" "$out/$abi/"
  cp "$out/$abi/libbindersg.so" "$out/$abi/libc++_shared.so" "$work/$abi/lib/$abi/"
  for role in server client; do
    python3 - "$source_dir/app/AndroidManifest.xml" "$work/manifest-$role.xml" "$role" <<'PY'
import sys,xml.etree.ElementTree as ET
ET.register_namespace('android','http://schemas.android.com/apk/res/android')
tree=ET.parse(sys.argv[1])
tree.getroot().set('package','com.gesher.bindersg.'+sys.argv[3])
tree.write(sys.argv[2],encoding='utf-8',xml_declaration=True)
PY
    "$build_tools/aapt2" link -o "$work/$abi-$role.apk" -I "$android_jar" \
      --manifest "$work/manifest-$role.xml" --min-sdk-version 29 --target-sdk-version 36
    (cd "$work/dex" && zip -q "$work/$abi-$role.apk" classes.dex)
    (cd "$work/$abi" && zip -q -0 "$work/$abi-$role.apk" lib/$abi/*.so)
    "$build_tools/zipalign" -f -p 4 "$work/$abi-$role.apk" "$out/binder-sg-$role-$abi.apk"
  done
done
# Verify payload identity in CI rather than assuming the packaging command
# selected the same library. The manifest also pins every consumed test file.
python3 "$source_dir/../../tools/binder/modes_manifest.py" seal --fixtures "$out"
