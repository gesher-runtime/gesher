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

javac -encoding UTF-8 --release 11 -d "$work/classes" "$root"/tests/dex/*.java
mkdir -p "$work/dex"
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 26 \
  --output "$work/dex" $(find "$work/classes" -name '*.class')

cp "$work/dex/classes.dex" "$out/hello.dex"
python3 - "$work/dex/classes.dex" "$out/hello.apk" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED) as z:
    z.write(sys.argv[1], "classes.dex")
PY

# libcore tests (tests/dex-libcore), apart from Hello, which the installer ships.
javac --release 11 -d "$work/classes-libcore" "$root"/tests/dex-libcore/*.java
mkdir -p "$work/dex-libcore"
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 26 \
  --output "$work/dex-libcore" $(find "$work/classes-libcore" -name '*.class')
cp "$work/dex-libcore/classes.dex" "$out/libcore-tests.dex"

# Compiled code and nterp (tests/dex-codegen): the benchmark and the stress test.
javac --release 11 -d "$work/classes-codegen" "$root"/tests/dex-codegen/*.java
mkdir -p "$work/dex-codegen"
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 26 \
  --output "$work/dex-codegen" $(find "$work/classes-codegen" -name '*.class')
cp "$work/dex-codegen/classes.dex" "$out/codegen-tests.dex"

# framework.jar on the boot class path (tests/dex-framework).
javac --release 11 -d "$work/classes-framework" "$root"/tests/dex-framework/*.java
mkdir -p "$work/dex-framework"
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 26 \
  --output "$work/dex-framework" $(find "$work/classes-framework" -name '*.class')
cp "$work/dex-framework/classes.dex" "$out/framework-tests.dex"

# The framework-natives tests (tests/dex-natives): gesher.NativeTest,
# gesher.LooperTest and gesher.ResourcesTest. LooperTest runs the real Looper,
# Handler, Message, MessageQueue and HandlerThread from frameworks/base
# (MessageQueue as Soong's messagequeue-gen picks it for bp4a:
# CombinedMessageQueue); everything else of the framework they reference is a
# stand-in. The stand-ins in tests/dex-natives-in get their natives from the
# JNI tables (tests/gen_jni_standins.py). libcore's FileDescriptor.getInt$ is
# patched into java.base for javac only (tests/dex-natives-javabase); java.*
# is not dexed.
aosp=${AOSP_DIR:-$root/third_party/aosp}
fw=$aosp/frameworks/base
fwos=$fw/core/java/android/os
py=${PYTHON:-python3}
mkdir -p "$work/classes-natives" "$work/dex-natives" "$work/gen-natives"
"$py" "$root/tests/gen_jni_standins.py" "$fw" "$work/gen-natives" \
  $(find "$root/tests/dex-natives-in" -name '*.java.in')
javac -encoding UTF-8 -source 17 -target 17 -nowarn -proc:none \
  --patch-module "java.base=$root/tests/dex-natives-javabase" \
  -d "$work/classes-natives" \
  $(find "$root/tests/dex-natives" "$root/tests/dex-natives-javabase" "$work/gen-natives" \
      -name '*.java') \
  "$fwos"/{Looper,Handler,Message,HandlerThread,MessageHeap,MessageStack}.java \
  "$fwos/CombinedMessageQueue/MessageQueue.java"
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 36 \
  --output "$work/dex-natives" \
  $(find "$work/classes-natives" -name '*.class' -not -path "$work/classes-natives/java/*")
cp "$work/dex-natives/classes.dex" "$out/natives.dex"

# ResourcesTest's APK (tests/res-app), linked by aapt2 from Google's
# build-tools without android.jar: it uses its own attributes only.
BUILD_TOOLS=36.1
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  host=windows exe=.exe
  native() { cygpath -w "$1"; }
else
  host=linux exe=
  native() { echo "$1"; }
fi
tools=${BUILD_TOOLS_DIR:-$work/build-tools}
if [[ ! -x $tools/aapt2$exe ]]; then
  curl -fsSL -o "$work/build-tools.zip" \
    "https://dl.google.com/android/repository/build-tools_r${BUILD_TOOLS}_$host.zip"
  rm -rf "$work/build-tools.tmp" && mkdir -p "$work/build-tools.tmp" "$tools"
  "$py" -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
    "$(native "$work/build-tools.zip")" "$(native "$work/build-tools.tmp")"
  cp -r "$work/build-tools.tmp"/*/. "$tools/"
  chmod +x "$tools/aapt2$exe"
fi
(cd "$root/tests/res-app" &&
  "$tools/aapt2$exe" compile --dir res -o "$(native "$work/res.zip")" &&
  "$tools/aapt2$exe" link -o "$(native "$out/res.apk")" --manifest AndroidManifest.xml \
    -A assets "$(native "$work/res.zip")")

echo "test apps written to $out"
