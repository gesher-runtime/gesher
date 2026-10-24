#!/usr/bin/env bash
# The test jars ART's libdexfile gtests read (art-gtest-jars-<Name> in
# art/test/Android.bp), next to the test executables, as Soong installs them:
#
#   tests/art-gtests/build-test-jars.sh <out-dir>
#
# Needs a JDK; d8 comes from Google's Maven repository, as in
# tests/build-test-apps.sh. VerifierDeps is smali and is not built (its one
# test is excluded, docs/posix.md).
set -euo pipefail

R8_VERSION=9.5.26
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
aosp=${AOSP_DIR:-$root/third_party/aosp}
tests=$aosp/art/test
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

r8=${R8_JAR:-$work/r8.jar}
if [[ ! -f $r8 ]]; then
  curl -fsSL -o "$r8" \
    "https://dl.google.com/android/maven2/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar"
fi

zip_dex() {  # zip_dex <dex dir> <jar>: classes*.dex, deflated, as a java_library's jar
  python3 - "$1" "$2" <<'PY'
import os, sys, zipfile
src, jar = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(jar, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(src)):
        if name.startswith("classes") and name.endswith(".dex"):
            z.write(os.path.join(src, name), name)
PY
}

for name in Main Nested Lookup GetMethodSignature MultiDex; do
  mkdir -p "$work/$name/classes" "$work/$name/dex"
  javac -encoding UTF-8 --release 8 -nowarn -proc:none -d "$work/$name/classes" \
    $(find "$tests/$name" -name '*.java')
  if [[ $name == MultiDex ]]; then
    # dxflags of art-gtest-jars-MultiDex: the classes main.list names in
    # classes.dex, the rest in classes2.dex. (D8 takes a main-dex list only for
    # dex inputs, so each half is dexed on its own.)
    main=() rest=()
    for c in $(cd "$work/$name/classes" && find . -name '*.class' | sed 's|^\./||'); do
      if grep -qx "$c" "$tests/MultiDex/main.list"; then main+=("$work/$name/classes/$c")
      else rest+=("$work/$name/classes/$c"); fi
    done
    mkdir -p "$work/$name/dex2"
    java -cp "$r8" com.android.tools.r8.D8 --release --min-api 19 --output "$work/$name/dex" "${main[@]}"
    java -cp "$r8" com.android.tools.r8.D8 --release --min-api 19 --output "$work/$name/dex2" "${rest[@]}"
    mv "$work/$name/dex2/classes.dex" "$work/$name/dex/classes2.dex"
  else
    java -cp "$r8" com.android.tools.r8.D8 --release --min-api 26 --output "$work/$name/dex" \
      $(find "$work/$name/classes" -name '*.class')
  fi
  zip_dex "$work/$name/dex" "$out/art-gtest-jars-$name.jar"
done

# art-gtest-jars-MainEmptyUncompressed: Main/empty.dex, stored (soong_zip -j -L 0).
python3 - "$tests/Main/empty.dex" "$out/art-gtest-jars-MainEmptyUncompressed.jar" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_STORED) as z:
    z.write(sys.argv[1], "empty.dex")
PY

ls -la "$out"/art-gtest-jars-*
