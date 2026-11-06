#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd);shift
args=("$@");framework= core=
while [[ $# -gt 0 ]];do
  case $1 in --framework-classes) framework=$2;shift 2;;--core-classes) core=$2;shift 2;;--framework-res) shift 2;;*) exit 2;;esac
done
work=$(mktemp -d);work=$(cd "$work" && pwd -P)
packageevents_tmp=$(cd "${TMPDIR:-/tmp}" && pwd -P)
case "$work" in "$packageevents_tmp"/*) [[ "$packageevents_tmp" != / ]] || exit 2;;*) exit 2;;esac
trap 'rm -rf "$work"' EXIT
native(){ if command -v cygpath >/dev/null;then cygpath -w "$1";else printf '%s\n' "$1";fi; }
r8=${R8_JAR:-$work/r8.jar}
if [[ ! -f $r8 ]];then
  curl -fsSL -o "$r8" https://dl.google.com/android/maven2/com/android/tools/r8/9.5.26/r8-9.5.26.jar
fi
r8=$(cd "$(dirname "$r8")" && pwd -P)/$(basename "$r8")
export R8_JAR="$r8"
APP_DIR="$here/observer" bash "$here/../lifecycle/build.sh" "$out" "${args[@]}"
mv "$out/observer.apk" "$out/packageevents-observer.apk"
system=();[[ -z $core ]] || system=(--system=none --module-path "$(native "$core")")
mkdir -p "$work/classes" "$work/dex"
javac -source 17 -target 17 "${system[@]}" -encoding UTF-8 -XDstringConcat=inline -cp "$(native "$framework")" \
  -d "$(native "$work/classes")" "$(native "$here/control/PackageEventsControl.java")"
java -cp "$(native "$R8_JAR")" com.android.tools.r8.D8 --release --min-api 36 --output "$(native "$work/dex")" \
  "$(native "$work/classes/gesher/fixture/PackageEventsControl.class")"
cp "$work/dex/classes.dex" "$out/packageevents-control.dex"
