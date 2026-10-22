#!/usr/bin/env bash
# Build ART's boot class path jars (core-oj, core-libart, core-icu4j) from
# libcore and ICU.
#
#   tools/build-boot-jars.sh <out-dir>
#
# Mirrors libcore/JavaLibrary.bp: one `core-all` javac compile of libcore as
# the java.base module, split into core-oj and core-libart by Soong's file
# lists, each dexed with D8 into a jar with its resources. ICU4J
# (external/icu/android_icu4j/Android.bp's core-icu4j: android.icu and the
# libcore bridge) is compiled in the same javac run and split out too. Needs JDK 21
# (JAVA_HOME or PATH) and the AOSP sources from tools/fetch-aosp.sh.
set -euo pipefail

R8_VERSION=9.5.26
root=$(cd "$(dirname "$0")/.." && pwd)
aosp=${AOSP_DIR:-$root/third_party/aosp}
out=$(mkdir -p "$1" && cd "$1" && pwd)
work=${WORK_DIR:-$(mktemp -d)}
mkdir -p "$work"
lc=$aosp/libcore
bin=${JAVA_HOME:+$JAVA_HOME/bin/}
py=${PYTHON:-python3}

# Native tools on Windows take Windows paths and ';' as path separator.
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -m "$1"; }
  sep=';'
else
  native() { echo "$1"; }
  sep=':'
fi

# --- Generated sources: aconfig flags ---------------------------------------
gen=$work/gen
rm -rf "$gen" && mkdir -p "$gen"
values=$root/config/aconfig/art.values
for decl in art/build/flags/art-flags.aconfig art/build/flags/art-rw-flags.aconfig \
            libcore/libcore.aconfig external/icu/icu.aconfig; do
  "$py" "$root/tools/gen/aconfig_java.py" "$aosp/$decl" "$values" "$gen"
done

# --- Source lists ------------------------------------------------------------
bps=("$lc/JavaLibrary.bp" "$lc/openjdk_java_files.bp" "$lc/non_openjdk_java_files.bp")
"$py" "$root/tools/gen/bp_files.py" -o "$work/core-oj.txt" core_oj_java_files "${bps[@]}"
"$py" "$root/tools/gen/bp_files.py" -o "$work/core-libart.txt" core_libart_java_files "${bps[@]}"
"$py" "$root/tools/gen/bp_files.py" -o "$work/resources.txt" core-oj.java_resources "${bps[@]}"
find "$gen" -name '*.java' > "$work/gen.txt"
# core-libart's static_libs: the ART and libcore flag classes.
grep -E '/com/android/(art/flags|libcore)/Flags\.java$' "$work/gen.txt" > "$work/core-libart-flags.txt"
echo "$lc/luni/src/module/java/module-info.java" > "$work/module.txt"
# core-icu4j: core-repackaged-icu4j (android.icu) and the libcore bridge
# (com.android.icu, com.android.i18n), with ICU's flag class.
icu4j=$aosp/external/icu/android_icu4j
{ find "$icu4j/src/main/java/android/icu" "$icu4j/libcore_bridge/src/java" -name '*.java'
  grep -E '/com/android/icu/Flags\.java$' "$work/gen.txt"; } | sort > "$work/core-icu4j.txt"
find "$icu4j/resources" -type f | sort > "$work/core-icu4j-resources.txt"

# --- core-all ----------------------------------------------------------------
# Libraries libcore compiles against: javac takes from these source roots
# only the classes libcore references; they are not part of the jars.
roots=(
  "$aosp/external/icu/android_icu4j/src/main/java"
  "$aosp/external/icu/android_icu4j/libcore_bridge/src/java"
  "$aosp/external/okhttp/repackaged/android/src/main/java"
  "$aosp/external/okhttp/repackaged/okhttp/src/main/java"
  "$aosp/external/okhttp/repackaged/okhttp-urlconnection/src/main/java"
  "$aosp/external/okhttp/repackaged/okhttp-android-support/src/main/java"
  "$aosp/external/okhttp/repackaged/okio/okio/src/main/java"
  "$aosp/frameworks/libs/modules-utils/java"
  "$aosp/tools/platform-compat/java"
  "$root/java/compile-stubs"
  "$gen"
)
patch=""
for r in "${roots[@]}"; do patch+="${patch:+$sep}$(native "$r")"; done

argfile=$work/javac.args
: > "$argfile"
for list in core-oj core-libart core-icu4j gen module; do
  while read -r f; do native "$f"; done < "$work/$list.txt" >> "$argfile"
done

classes=$work/classes
rm -rf "$classes" && mkdir -p "$classes"
echo "javac: $(wc -l < "$argfile") files"
"${bin}javac" -J-Xmx4g -source 21 -target 21 --system=none \
  --patch-module "java.base=$patch" \
  -encoding UTF-8 -nowarn -Xmaxerrs 200 -XDstringConcat=inline \
  -d "$(native "$classes")" "@$(native "$argfile")"

# --- Split and dex ---------------------------------------------------------
split=$work/split
rm -rf "$split"
"$py" "$root/tools/gen/split_classes.py" "$classes" "$split" \
  core-oj="$work/core-oj.txt" core-libart="$work/core-libart.txt" \
  core-libart="$work/core-libart-flags.txt" core-icu4j="$work/core-icu4j.txt"

r8=${R8_JAR:-$work/r8-$R8_VERSION.jar}
if [[ ! -f $r8 ]]; then
  curl -fsSL -o "$r8" \
    "https://dl.google.com/android/maven2/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar"
fi

for jar in core-oj core-libart core-icu4j; do
  dex=$work/dex/$jar
  rm -rf "$dex" && mkdir -p "$dex"
  find "$split/$jar" -name '*.class' > "$work/$jar.classes"
  while read -r f; do native "$f"; done < "$work/$jar.classes" > "$work/$jar.d8args"
  "${bin}java" -Xmx4g -cp "$(native "$r8")" com.android.tools.r8.D8 \
    --release --min-api 36 --android-platform-build \
    --output "$(native "$dex")" "@$(native "$work/$jar.d8args")"
  res=()
  [[ $jar == core-oj ]] && res=(--resources "$work/resources.txt")
  [[ $jar == core-icu4j ]] && res=(--resources "$work/core-icu4j-resources.txt"
                                   --resource-dir "$icu4j/resources")
  "$py" "$root/tools/gen/make_jar.py" "$out/$jar.jar" "$dex" "${res[@]}"
done
ls -la "$out"
