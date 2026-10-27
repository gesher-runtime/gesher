#!/usr/bin/env bash
# CI-only real DEX library jars plus an APK with direct, separately loaded calls.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
framework= core=
args=("$@")
while [[ $# -gt 0 ]]; do
  case $1 in
    --framework-classes) framework=$2; shift 2 ;;
    --core-classes) core=$2; shift 2 ;;
    --framework-res) shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[[ -n $framework ]] || { echo '--framework-classes is required' >&2; exit 2; }
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
  native_list() { cygpath -w -f "$1" > "$1.w" && mv "$1.w" "$1"; }
  separator=';'
else
  native() { echo "$1"; }
  native_list() { :; }
  separator=':'
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
r8=${R8_JAR:-$work/r8.jar}
if [[ ! -f $r8 ]]; then
  curl -fsSL -o "$r8" https://dl.google.com/android/maven2/com/android/tools/r8/9.5.26/r8-9.5.26.jar
fi
system=()
if [[ -n $core ]]; then system=(--system=none --module-path "$(native "$core")"); fi
for version in base-v1 base-v2 entry; do
  mkdir -p "$work/$version"
  find "$root/tests/shared-libraries/$version" -name '*.java' > "$work/sources.txt"
  native_list "$work/sources.txt"
  javac -source 17 -target 17 "${system[@]}" -encoding UTF-8 -nowarn -Xlint:-options \
    -XDstringConcat=inline -cp "$(native "$work/base-v1")" \
    -d "$(native "$work/$version")" @"$(native "$work/sources.txt")"
  find "$work/$version" -name '*.class' > "$work/dex.txt"
  native_list "$work/dex.txt"
  java -cp "$(native "$r8")" com.android.tools.r8.D8 --release --min-api 36 \
    --output "$(native "$out/shared-$version.jar")" @"$(native "$work/dex.txt")"
done
# The app's javac sees library classes; D8 packages only the APK's own classes.
classpath="$(native "$framework")$separator$(native "$work/base-v1")$separator$(native "$work/entry")"
appArgs=()
for ((i=0; i<${#args[@]}; ++i)); do
  if [[ ${args[i]} == --framework-classes ]]; then
    appArgs+=(--framework-classes "$classpath")
    ((i+=1))
  else
    appArgs+=("${args[i]}")
  fi
done
APP_DIR="$here" R8_JAR="$r8" "$root/tests/apps/lifecycle/build.sh" "$out" "${appArgs[@]}"
# Same package, changed required library: installer must preserve the working old APK.
mkdir -p "$work/sharedlibrarybad"
cp -R "$here/java" "$here/res" "$work/sharedlibrarybad/"
sed -e 's/gesher.shared.entry/gesher.shared.missing/' -e 's/versionCode="1"/versionCode="2"/' \
  "$here/AndroidManifest.xml" > "$work/sharedlibrarybad/AndroidManifest.xml"
APP_DIR="$work/sharedlibrarybad" R8_JAR="$r8" "$root/tests/apps/lifecycle/build.sh" "$out" "${appArgs[@]}"
echo "shared Java jars, sharedlibraryapp.apk and sharedlibrarybad.apk written to $out"
