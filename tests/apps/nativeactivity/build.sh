#!/usr/bin/env bash
# Build nativeactivity.apk, gesher-app's test app whose Activity is
# android.app.NativeActivity (tests/apps/nativeactivity): no Java code
# (hasCode="false"), lib/x86_64/libnativeactivity.so written with the NDK's
# android_native_app_glue.
#
#   tests/apps/nativeactivity/build.sh <out-dir> --ndk <NDK dir> --android-jar <android.jar> \
#       --build-tools <build-tools dir>
#
# The NDK builds jni/main.c and the glue's android_native_app_glue.c (API 33:
# AChoreographer_postVsyncCallback) into libnativeactivity.so against
# libandroid, libEGL, libGLESv2 and liblog; aapt2 links the manifest,
# resources and assets against the SDK's android.jar, and the APK is
# zip-aligned with its .so stored uncompressed. Unsigned: gesher-app does not
# verify signatures. Runs on the Linux runner (.github/workflows/native-libs.yml).
set -euo pipefail

app=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
ndk= android_jar= tools=
while [[ $# -gt 0 ]]; do
  case $1 in
    --ndk) ndk=$2; shift 2 ;;
    --android-jar) android_jar=$2; shift 2 ;;
    --build-tools) tools=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [[ -z $ndk || -z $android_jar || -z $tools ]]; then
  echo "usage: build.sh <out-dir> --ndk <dir> --android-jar <jar> --build-tools <dir>" >&2
  exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Native code: the glue as the NDK ships it (its own warnings are its own),
# the app's code with -Werror. ANativeActivity_onCreate is the glue's; -u keeps
# it, as the NDK's build systems do.
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
glue=$ndk/sources/android/native_app_glue
cc="$llvm/bin/x86_64-linux-android33-clang"
mkdir -p "$work/apk/lib/x86_64" "$work/obj"
"$cc" -c -fPIC -O2 -fstack-protector-strong -w -o "$work/obj/glue.o" "$glue/android_native_app_glue.c"
"$cc" -c -fPIC -O2 -fstack-protector-strong -Wall -Wextra -Werror -I "$glue" \
  -o "$work/obj/main.o" "$app/jni/main.c"
"$cc" -shared -fPIC -Wl,-soname,libnativeactivity.so -u ANativeActivity_onCreate \
  -o "$work/apk/lib/x86_64/libnativeactivity.so" "$work/obj/main.o" "$work/obj/glue.o" \
  -landroid -lEGL -lGLESv2 -llog
echo "--- libnativeactivity.so: needed, exports, and what it imports ---"
"$llvm/bin/llvm-readelf" -d "$work/apk/lib/x86_64/libnativeactivity.so" | grep -E 'NEEDED|SONAME'
"$llvm/bin/llvm-readelf" --dyn-syms -W "$work/apk/lib/x86_64/libnativeactivity.so" |
  awk '$7 != "UND" && $5 == "GLOBAL" {print $8}' | sort -u | tr '\n' ' '
echo
"$llvm/bin/llvm-readelf" --dyn-syms -W "$work/apk/lib/x86_64/libnativeactivity.so" |
  awk '$7 == "UND" && $8 != "" {print $8}' | sort -u | tr '\n' ' '
echo

# Resources, manifest and assets; no classes.dex (hasCode="false").
"$tools/aapt2" compile --dir "$app/res" -o "$work/res.zip"
"$tools/aapt2" link -o "$work/unaligned.apk" -I "$android_jar" \
  --manifest "$app/AndroidManifest.xml" --min-sdk-version 33 --target-sdk-version 36 \
  -A "$app/assets" "$work/res.zip"
(cd "$work/apk" && zip -q -0 "$work/unaligned.apk" lib/x86_64/*.so)
"$tools/zipalign" -f -p 4 "$work/unaligned.apk" "$out/nativeactivity.apk"
unzip -l "$out/nativeactivity.apk"
echo "nativeactivity.apk written to $out"
