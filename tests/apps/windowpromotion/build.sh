#!/usr/bin/env bash
# Two synthetic APKs plus a host-only control DEX. No Google packages or accounts.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
app_classes= framework= core=
app_args=()
while [[ $# -gt 0 ]]; do
  case $1 in
    --app-classes) app_classes=$2; shift 2 ;;
    --framework-classes) framework=$2; app_args+=("$1" "$2"); shift 2 ;;
    --core-classes) core=$2; app_args+=("$1" "$2"); shift 2 ;;
    --framework-res) app_args+=("$1" "$2"); shift 2 ;;
    *) echo "unknown fixture build option: $1" >&2; exit 2 ;;
  esac
done
[[ -n $app_classes && -n $framework ]] || { echo 'app and framework classes required' >&2; exit 2; }
work=$(mktemp -d)
work=$(cd "$work" && pwd -P)
promotion_tmp_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
case "$work" in
  "$promotion_tmp_root"/*) [[ "$promotion_tmp_root" != / ]] || exit 2 ;;
  *) echo 'temporary build directory escaped its intended parent' >&2; exit 2 ;;
esac
trap 'rm -rf "$work"' EXIT
r8=${R8_JAR:-$work/r8.jar}
if [[ ! -f $r8 ]]; then
  curl -fsSL -o "$r8" https://dl.google.com/android/maven2/com/android/tools/r8/9.5.26/r8-9.5.26.jar
fi
r8=$(cd "$(dirname "$r8")" && pwd -P)/$(basename "$r8")
export R8_JAR="$r8"
for app in target source; do
  APP_DIR="$here/$app" bash "$here/../lifecycle/build.sh" "$out" "${app_args[@]}"
  mv "$out/$app.apk" "$out/windowpromotion-$app.apk"
done
native() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else printf '%s\n' "$1"; fi; }
sep=:; if command -v cygpath >/dev/null; then sep=';'; fi
system=(); [[ -z $core ]] || system=(--system=none --module-path "$(native "$core")")
mkdir -p "$work/classes" "$work/dex"
javac -source 17 -target 17 "${system[@]}" -encoding UTF-8 -XDstringConcat=inline \
  -cp "$(native "$framework")$sep$(native "$app_classes")" -d "$(native "$work/classes")" \
  "$(native "$here/control/WindowPromotionControl.java")"
java -cp "$(native "$r8")" com.android.tools.r8.D8 --release --min-api 36 \
  --output "$(native "$work/dex")" "$(native "$work/classes/WindowPromotionControl.class")"
cp "$work/dex/classes.dex" "$out/windowpromotion-control.dex"
