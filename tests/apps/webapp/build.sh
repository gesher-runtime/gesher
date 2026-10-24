#!/usr/bin/env bash
# Build webapp.apk, the WebView test app (tests/apps/webapp).
#
#   tests/apps/webapp/build.sh <out-dir> --framework-res <framework-res.apk> \
#       --framework-classes <dir|jar> [--core-classes <dir>]
#
# aapt2 compiles the resources and links them with the manifest against
# framework-res.apk, which also writes R.java; javac compiles the app against
# framework.jar's classes (and libcore's java.base, as app/build.sh does); d8 dexes it;
# the dex is added to the APK and the APK is zip-aligned. It is not signed: gesher-app
# does not verify signatures.
#
# aapt2 and zipalign come from Google's build-tools r36.1 (BUILD_TOOLS_DIR to use an
# unpacked copy), d8 from Google's Maven repository (R8_JAR to use a local copy).
# Runs on Linux and in Git Bash on Windows.
set -euo pipefail

BUILD_TOOLS=36.1
R8_VERSION=9.5.26
app=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
framework_res= framework= core=
while [[ $# -gt 0 ]]; do
  case $1 in
    --framework-res) framework_res=$2; shift 2 ;;
    --framework-classes) framework=$2; shift 2 ;;
    --core-classes) core=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [[ -z $framework_res || -z $framework ]]; then
  echo "usage: build.sh <out-dir> --framework-res <apk> --framework-classes <dir|jar>" \
    "[--core-classes <dir>]" >&2
  exit 2
fi

# Native tools on Windows take Windows paths.
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
  native_list() { cygpath -w -f "$1" > "$1.w" && mv "$1.w" "$1"; }
  host=windows exe=.exe
else
  native() { echo "$1"; }
  native_list() { :; }
  host=linux exe=
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

tools=${BUILD_TOOLS_DIR:-}
if [[ -z $tools ]]; then
  tools=$work/build-tools
  curl -fsSL -o "$work/build-tools.zip" \
    "https://dl.google.com/android/repository/build-tools_r${BUILD_TOOLS}_$host.zip"
  mkdir -p "$work/build-tools.tmp"
  (cd "$work/build-tools.tmp" && jar -xf "$(native "$work/build-tools.zip")")
  mv "$work/build-tools.tmp"/* "$tools"
  chmod +x "$tools"/aapt2* "$tools"/zipalign* 2>/dev/null || true
fi
aapt2=$tools/aapt2$exe
zipalign=$tools/zipalign$exe

r8=${R8_JAR:-$work/r8.jar}
if [[ ! -f $r8 ]]; then
  curl -fsSL -o "$r8" \
    "https://dl.google.com/android/maven2/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar"
fi

# Resources and manifest. aapt2 wants res/<type>/ paths, relative on Windows.
(cd "$app" && "$aapt2" compile --dir res -o "$(native "$work/res.zip")")
mkdir -p "$work/gen"
"$aapt2" link -o "$(native "$work/unaligned.apk")" -I "$(native "$framework_res")" \
  --manifest "$(native "$app/AndroidManifest.xml")" -A "$(native "$app/assets")" --java "$(native "$work/gen")" \
  --min-sdk-version 36 --target-sdk-version 36 "$(native "$work/res.zip")"

# Code.
system=()
if [[ -n $core ]]; then
  system=(--system=none --module-path "$(native "$core")")
fi
find "$app/java" "$work/gen" -name '*.java' > "$work/sources.txt"
native_list "$work/sources.txt"
mkdir -p "$work/classes" "$work/dex"
javac -source 17 -target 17 "${system[@]}" -encoding UTF-8 -nowarn -Xlint:-options -XDstringConcat=inline \
  -cp "$(native "$framework")" -d "$(native "$work/classes")" @"$(native "$work/sources.txt")"
(cd "$work" && find classes -name '*.class' > dex.txt &&
  java -cp "$(native "$r8")" com.android.tools.r8.D8 --release --min-api 36 \
    --output dex @dex.txt)

# The dex into the APK, then 4-byte alignment.
(cd "$work/dex" && jar -u -M -f "$(native "$work/unaligned.apk")" classes.dex)
"$zipalign" -f -p 4 "$(native "$work/unaligned.apk")" "$(native "$out/webapp.apk")"
echo "webapp.apk written to $out"
