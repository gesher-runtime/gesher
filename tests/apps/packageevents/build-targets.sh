#!/usr/bin/env bash
# Ordinary signed same-certificate versions for actual publication/cache tests.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd); shift
work=$(mktemp -d)
work=$(cd "$work" && pwd -P)
packageevents_tmp=$(cd "${TMPDIR:-/tmp}" && pwd -P)
case "$work" in "$packageevents_tmp"/*) [[ "$packageevents_tmp" != / ]] || exit 2;; *) exit 2;; esac
trap 'rm -rf "$work"' EXIT
native(){ if command -v cygpath >/dev/null; then cygpath -w "$1"; else printf '%s\n' "$1"; fi; }
for version in 1 2; do
  cp -R "$here/target" "$work/v$version"
  sed "s/android:versionCode=\"1\"/android:versionCode=\"$version\"/;s/android:versionName=\"1\"/android:versionName=\"$version\"/" \
    "$here/target/AndroidManifest.xml" > "$work/v$version/AndroidManifest.xml"
  APP_DIR="$work/v$version" bash "$here/../lifecycle/build.sh" "$out" "$@"
done
keytool -genkeypair -keystore "$(native "$work/key.p12")" -storetype PKCS12 -storepass fixture-only \
  -alias fixture -keyalg RSA -keysize 2048 -validity 36500 -dname 'CN=Gesher package event fixture' > /dev/null 2>&1
for version in 1 2; do
  java -jar "$(native "$BUILD_TOOLS_DIR/lib/apksigner.jar")" sign \
    --ks "$(native "$work/key.p12")" --ks-pass pass:fixture-only \
    --out "$(native "$out/packageevents-target-v$version.apk")" "$(native "$out/v$version.apk")"
done
