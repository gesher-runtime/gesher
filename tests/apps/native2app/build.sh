#!/usr/bin/env bash
# Build native2app.apk (tests/apps/native2app), the native2 track's test app.
#
#   tests/apps/native2app/build.sh <out-dir> <apk-name> <abi> --ndk <NDK dir> \
#       --android-jar <android.jar> --build-tools <build-tools dir> --r8 <r8.jar>
#
# <abi> is x86_64 or arm64-v8a: the NDK builds tests/native2/native2_test.cpp
# into lib/<abi>/libnative2.so against its libc++_shared.so (in the APK too),
# and tests/native2/native2_helper.c into libnative2_helper.so, which the
# library dlopen()s by its bare name. As tests/apps/nativeapp/build.sh
# otherwise: unsigned, .so files stored and page-aligned. Runs on the Linux
# runner (.github/workflows/native2.yml).
set -euo pipefail

app=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$app/../../native2" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
name=$2
abi=$3
shift 3
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
case $abi in
  x86_64) triple=x86_64-linux-android ;;
  arm64-v8a) triple=aarch64-linux-android ;;
  *) echo "unknown ABI $abi" >&2; exit 2 ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
lib=$work/apk/lib/$abi
mkdir -p "$lib"
"$llvm/bin/${triple}29-clang++" -std=c++20 -shared -fPIC -O2 -fstack-protector-strong -Wall -Werror \
  -Wl,-soname,libnative2.so -o "$lib/libnative2.so" "$src/native2_test.cpp" \
  -lz -lEGL -lOpenSLES -laaudio -landroid -llog -ldl
"$llvm/bin/${triple}29-clang" -shared -fPIC -O2 -Wl,-soname,libnative2_helper.so \
  -o "$lib/libnative2_helper.so" "$src/native2_helper.c"
cp "$llvm/sysroot/usr/lib/$triple/libc++_shared.so" "$lib/"

"$tools/aapt2" compile --dir "$app/res" -o "$work/res.zip"
"$tools/aapt2" link -o "$work/unaligned.apk" -I "$android_jar" \
  --manifest "$app/AndroidManifest.xml" --min-sdk-version 29 --target-sdk-version 36 "$work/res.zip"

mkdir -p "$work/classes" "$work/dex"
javac --release 11 -encoding UTF-8 -cp "$android_jar" -d "$work/classes" $(find "$app/java" -name '*.java')
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 29 --lib "$android_jar" \
  --output "$work/dex" $(find "$work/classes" -name '*.class')

(cd "$work/dex" && zip -q "$work/unaligned.apk" classes.dex)
(cd "$work/apk" && zip -q -0 "$work/unaligned.apk" lib/$abi/*.so)
"$tools/zipalign" -f -p 4 "$work/unaligned.apk" "$out/$name"
unzip -l "$out/$name"
