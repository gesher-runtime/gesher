#!/usr/bin/env bash
# Separate JNI-owner fixture DEX; these server-named classes never enter app.jar.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
work=$(mktemp -d)
work=$(cd "$work" && pwd -P)
chooser_tmp_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
case "$work" in
  "$chooser_tmp_root"/*) [[ "$chooser_tmp_root" != / ]] || exit 2 ;;
  *) echo 'temporary build directory escaped its intended parent' >&2; exit 2 ;;
esac
trap 'rm -rf "$work"' EXIT
r8=${2:-${R8_JAR:-$work/r8.jar}}
if [[ ! -f $r8 ]]; then
  curl -fsSL -o "$r8" https://dl.google.com/android/maven2/com/android/tools/r8/9.5.26/r8-9.5.26.jar
fi
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
else
  native() { echo "$1"; }
fi
mkdir -p "$work/classes" "$work/dex"
find "$here" -name '*.java' > "$work/sources.txt"
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  cygpath -w -f "$work/sources.txt" > "$work/sources.windows.txt"
  mv "$work/sources.windows.txt" "$work/sources.txt"
fi
javac --release 11 -encoding UTF-8 -d "$(native "$work/classes")" @"$(native "$work/sources.txt")"
(cd "$work" && find classes -name '*.class') > "$work/dex.txt"
(cd "$work" && java -cp "$(native "$r8")" com.android.tools.r8.D8 --release --min-api 36 \
  --output "$(native "$work/dex")" @dex.txt)
cp "$work/dex/classes.dex" "$out/account-chooser-native-tests.dex"
