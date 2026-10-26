#!/usr/bin/env bash
# Package the already-built exec2 PIE programs and exec3 Java tests into an APK.
set -euo pipefail
app=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
android_jar= tools= r8=
while [[ $# -gt 0 ]]; do
  case $1 in
    --android-jar) android_jar=$2; shift 2 ;;
    --build-tools) tools=$2; shift 2 ;;
    --r8) r8=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [[ -z $android_jar || -z $tools || -z $r8 ]]; then
  echo "usage: build.sh <out> --android-jar <jar> --build-tools <dir> --r8 <jar>" >&2
  exit 2
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/classes" "$work/dex" "$work/apk/lib/x86_64"
cp "$out/libexec3jni.so" "$out/libexec2child.so" "$out/libexec3tls.so" "$work/apk/lib/x86_64/"
"$tools/aapt2" link -o "$work/unaligned.apk" -I "$android_jar" \
  --manifest "$app/AndroidManifest.xml" --min-sdk-version 26 --target-sdk-version 28
javac --release 11 -encoding UTF-8 -cp "$android_jar" -d "$work/classes" \
  "$app/java/gesher/ExecActivity.java" "$app/../java/gesher/Exec3Test.java" \
  "$app/../java/gesher/Exec3ProcessTest.java" "$app/../java/gesher/Exec3PropertiesTest.java" \
  "$app/../java/gesher/Exec3LogTest.java" "$app/../java/gesher/Exec3CommandsTest.java"
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 26 --lib "$android_jar" \
  --output "$work/dex" $(find "$work/classes" -name '*.class')
(cd "$work/dex" && zip -q "$work/unaligned.apk" classes.dex)
(cd "$work/apk" && zip -q -0 "$work/unaligned.apk" lib/x86_64/*.so)
"$tools/zipalign" -f -p 4 "$work/unaligned.apk" "$out/exec3.apk"
unzip -l "$out/exec3.apk"

# A separate installed package lets pm/am checks observe its actual lifecycle.
mkdir -p "$work/target-classes" "$work/target-dex"
"$tools/aapt2" link -o "$work/target.apk" -I "$android_jar" \
  --manifest "$app/target/AndroidManifest.xml" --min-sdk-version 26 --target-sdk-version 28
javac --release 11 -encoding UTF-8 -cp "$android_jar" -d "$work/target-classes" \
  "$app/target/java/gesher/TargetActivity.java" "$app/target/java/gesher/TargetReceiver.java"
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 26 --lib "$android_jar" \
  --output "$work/target-dex" $(find "$work/target-classes" -name '*.class')
(cd "$work/target-dex" && zip -q "$work/target.apk" classes.dex)
"$tools/zipalign" -f -p 4 "$work/target.apk" "$out/exec3target.apk"
unzip -l "$out/exec3target.apk"
