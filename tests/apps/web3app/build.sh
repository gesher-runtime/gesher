#!/usr/bin/env bash
# Build web3app.apk, the web3 track's test app (tests/apps/web3app, ADR 0058), with
# androidx.webkit from Google's Maven repository (pinned by SHA-256) compiled in, as
# apps ship it.
#
#   tests/apps/web3app/build.sh <out-dir> --framework-res <framework-res.apk> \
#       --framework-classes <dir|jar> [--core-classes <dir>]
#
# As tests/apps/webapp/build.sh (aapt2 with the assets, javac, d8, zipalign), plus the
# AARs' classes.jar on javac's class path and in d8's input. Not signed: gesher-app does
# not verify signatures. Runs on Linux and in Git Bash on Windows.
set -euo pipefail

BUILD_TOOLS=36.1
R8_VERSION=9.5.26
MAVEN=https://dl.google.com/android/maven2
# androidx.webkit and what its classes use at run time (androidx.core's Pair and
# PackageInfoCompat); its annotation dependencies are compile-time only.
AARS=(
  "androidx/webkit/webkit/1.18.0/webkit-1.18.0.aar f15a0bd7d51f4bb8dffdd3a2cf5829f9a7de9ad4ba2682fa85d619032050581f"
  "androidx/core/core/1.1.0/core-1.1.0.aar 76c7cfbe596fe3c09a6983bf1c89e889299c08ac9a3b52ce5182a088d056647e"
)
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

if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
  native_list() { cygpath -w -f "$1" > "$1.w" && mv "$1.w" "$1"; }
  host=windows exe=.exe sep=';'
else
  native() { echo "$1"; }
  native_list() { :; }
  host=linux exe= sep=':'
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
  curl -fsSL --retry 5 --retry-all-errors -o "$r8" \
    "$MAVEN/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar"
fi

# The AARs, checked, and their classes.
mkdir -p "$work/aar"
libs=()
for entry in "${AARS[@]}"; do
  path=${entry% *} sum=${entry#* }
  name=$(basename "$path" .aar)
  curl -fsSL --retry 5 --retry-all-errors -o "$work/aar/$name.aar" "$MAVEN/$path"
  echo "$sum  $work/aar/$name.aar" | sha256sum -c - >/dev/null || {
    echo "$name.aar: SHA-256 mismatch" >&2; exit 1; }
  mkdir -p "$work/aar/$name"
  (cd "$work/aar/$name" && jar -xf "$(native "$work/aar/$name.aar")" classes.jar)
  libs+=("$work/aar/$name/classes.jar")
done
classpath=$(native "$framework")
for l in "${libs[@]}"; do classpath="$classpath$sep$(native "$l")"; done

# Resources, manifest and assets.
(cd "$app" && "$aapt2" compile --dir res -o "$(native "$work/res.zip")")
mkdir -p "$work/gen"
"$aapt2" link -o "$(native "$work/unaligned.apk")" -I "$(native "$framework_res")" \
  --manifest "$(native "$app/AndroidManifest.xml")" -A "$(native "$app/assets")" --java "$(native "$work/gen")" \
  --min-sdk-version 36 --target-sdk-version 36 "$(native "$work/res.zip")"

# Code: the app's, then dexed with the AARs' classes.
system=()
if [[ -n $core ]]; then
  system=(--system=none --module-path "$(native "$core")")
fi
find "$app/java" "$work/gen" -name '*.java' > "$work/sources.txt"
native_list "$work/sources.txt"
mkdir -p "$work/classes" "$work/dex"
javac -source 17 -target 17 "${system[@]}" -encoding UTF-8 -nowarn -Xlint:-options -XDstringConcat=inline \
  -cp "$classpath" -d "$(native "$work/classes")" @"$(native "$work/sources.txt")"
(cd "$work/classes" && jar -c -f "$(native "$work/app.jar")" .)
d8_in=("$(native "$work/app.jar")")
for l in "${libs[@]}"; do d8_in+=("$(native "$l")"); done
java -cp "$(native "$r8")" com.android.tools.r8.D8 --release --min-api 36 \
  --output "$(native "$work/dex")" "${d8_in[@]}" 2>&1 \
  | grep -v -E '^Warning|Type .* was not found|missing class|^$' || true
test -f "$work/dex/classes.dex"

(cd "$work/dex" && jar -u -M -f "$(native "$work/unaligned.apk")" classes*.dex)
"$zipalign" -f -p 4 "$(native "$work/unaligned.apk")" "$(native "$out/web3app.apk")"
echo "web3app.apk written to $out (androidx.webkit 1.18.0)"
