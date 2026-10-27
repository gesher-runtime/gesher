#!/usr/bin/env bash
# Build surfacegl.apk (tests/apps/surfacegl, surfaces track): EGL/GLES 2 from an NDK
# library into a SurfaceView.
#
#   tests/apps/surfacegl/build.sh <out-dir> --ndk <NDK dir> --android-jar <android.jar> \
#       --build-tools <build-tools dir> --r8 <r8.jar>
#
# The NDK builds jni/surfacegl.c for x86_64 and arm64-v8a (C, against
# libandroid, libEGL and libGLESv2); aapt2 links the manifest and resources
# against the SDK's android.jar, javac and d8 compile the Activity, and the APK
# is zip-aligned with its .so stored uncompressed. Unsigned: gesher-app does
# not verify signatures. Runs on the Linux runner
# (.github/workflows/surfaces.yml).
set -euo pipefail

app=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
ndk= android_jar= tools= r8=
while [[ $# -gt 0 ]]; do
  case $1 in
    --ndk) ndk=$2; shift 2 ;;
    --android-jar) android_jar=$2; shift 2 ;;
    --build-tools) tools=$2; shift 2 ;;
    --r8) r8=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [[ -z $ndk || -z $android_jar || -z $tools || -z $r8 ]]; then
  echo "usage: build.sh <out-dir> --ndk <dir> --android-jar <jar> --build-tools <dir> --r8 <jar>" >&2
  exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Native code.
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
for abi in x86_64 arm64-v8a; do
  triple=x86_64-linux-android
  suffix=
  if [[ $abi == arm64-v8a ]]; then triple=aarch64-linux-android; suffix=-arm64; fi
  mkdir -p "$work/apk/lib/$abi"
  "$llvm/bin/${triple}26-clang" -shared -fPIC -O2 -fstack-protector-strong \
    -Wall -Wextra -Werror -fvisibility=hidden \
    -Wl,-soname,libsurfacegl.so -o "$work/apk/lib/$abi/libsurfacegl.so" "$app/jni/surfacegl.c" \
    -landroid -lEGL -lGLESv2 -llog
  cp "$work/apk/lib/$abi/libsurfacegl.so" "$out/libsurfacegl${suffix}.so"
  echo "--- libsurfacegl.so $abi: needed, and what it imports ---"
  "$llvm/bin/llvm-readelf" -d "$work/apk/lib/$abi/libsurfacegl.so" | grep -E 'NEEDED|SONAME'
  "$llvm/bin/llvm-readelf" --dyn-syms -W "$work/apk/lib/$abi/libsurfacegl.so" |
    awk '$7 == "UND" && $8 != "" {print $4, $8}' | sort -u | tr '\n' ' '
  echo
done

# Resources and manifest.
"$tools/aapt2" compile --dir "$app/res" -o "$work/res.zip"
"$tools/aapt2" link -o "$work/unaligned.apk" -I "$android_jar" \
  --manifest "$app/AndroidManifest.xml" --min-sdk-version 26 --target-sdk-version 36 \
  "$work/res.zip"

# Code.
mkdir -p "$work/classes" "$work/dex"
javac --release 11 -encoding UTF-8 -cp "$android_jar" -d "$work/classes" \
  $(find "$app/java" -name '*.java')
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 26 --lib "$android_jar" \
  --output "$work/dex" $(find "$work/classes" -name '*.class')

# Into the APK: the dex compressed, the library stored; then alignment
# (-p: .so files on 4 KiB page boundaries).
(cd "$work/dex" && zip -q "$work/unaligned.apk" classes.dex)
for abi in x86_64 arm64-v8a; do
  name=surfacegl
  if [[ $abi == arm64-v8a ]]; then name=surfacegl-arm64; fi
  cp "$work/unaligned.apk" "$work/$name.apk"
  (cd "$work/apk" && zip -q -0 "$work/$name.apk" "lib/$abi/libsurfacegl.so")
  "$tools/zipalign" -f -p 4 "$work/$name.apk" "$out/$name.apk"
  unzip -l "$out/$name.apk"
  echo "$name.apk written to $out"
done
