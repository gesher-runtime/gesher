#!/usr/bin/env bash
# Build services-tests.dex: the system services' tests (tests/dex-services),
# compiled against framework.jar's classes as app/build.sh compiles gesher-app.
#
#   tests/dex-services/build.sh <out-dir> --framework-classes <dir|jar> [--core-classes <dir>]
#
# Needs a JDK 17+ and curl; d8 comes from Google's Maven repository (R8_JAR to use a
# local copy). Runs on Linux and in Git Bash on Windows.
set -euo pipefail

R8_VERSION=9.5.26
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
framework= core=
while [[ $# -gt 0 ]]; do
  case $1 in
    --framework-classes) framework=$2; shift 2 ;;
    --core-classes) core=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [[ -z $framework ]]; then
  echo "usage: build.sh <out-dir> --framework-classes <dir|jar> [--core-classes <dir>]" >&2
  exit 2
fi

if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
  native_list() { cygpath -w -f "$1" > "$1.w" && mv "$1.w" "$1"; }
else
  native() { echo "$1"; }
  native_list() { :; }
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

r8=${R8_JAR:-$work/r8.jar}
if [[ ! -f $r8 ]]; then
  curl -fsSL -o "$r8" \
    "https://dl.google.com/android/maven2/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar"
fi

system=()
if [[ -n $core ]]; then
  system=(--system=none --module-path "$(native "$core")")
  while IFS= read -r pkg; do
    system+=(--add-exports "java.base/$pkg=ALL-UNNAMED")
  done < <(cd "$core" && find . -name '*.class' ! -name module-info.class -printf '%h\n' \
             | sed 's|^\./||; s|/|.|g' | sort -u)
fi
mkdir -p "$work/classes" "$work/dex"
find "$here" -name '*.java' > "$work/sources.txt"
native_list "$work/sources.txt"
javac -source 17 -target 17 "${system[@]}" -encoding UTF-8 -nowarn -Xlint:-options -XDstringConcat=inline \
  -cp "$(native "$framework")" -d "$(native "$work/classes")" @"$(native "$work/sources.txt")"
( cd "$work" && find classes -name '*.class' ) > "$work/dex.txt"
(cd "$work" && java -cp "$(native "$r8")" com.android.tools.r8.D8 --release --min-api 36 \
  --output "$(native "$work/dex")" @dex.txt)
cp "$work/dex/classes.dex" "$out/services-tests.dex"
echo "services-tests.dex written to $out"
