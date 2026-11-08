#!/usr/bin/env bash
# Real isolated feature DEX/resources and malformed cluster inputs, built with aapt2.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
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
[[ -n $framework_res && -n $framework && -n ${BUILD_TOOLS_DIR:-} ]] || {
  echo 'Supply --framework-res, --framework-classes and BUILD_TOOLS_DIR' >&2; exit 2;
}
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
  native_list() { cygpath -w -f "$1" > "$1.w" && mv "$1.w" "$1"; }
  separator=';' exe=.exe
else
  native() { echo "$1"; }
  native_list() { :; }
  separator=':' exe=
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
aapt2=$BUILD_TOOLS_DIR/aapt2$exe
zipalign=$BUILD_TOOLS_DIR/zipalign$exe
r8=${R8_JAR:-$work/r8.jar}
if [[ ! -f $r8 ]]; then
  curl -fsSL -o "$r8" https://dl.google.com/android/maven2/com/android/tools/r8/9.5.26/r8-9.5.26.jar
fi
system=()
if [[ -n $core ]]; then system=(--system=none --module-path "$(native "$core")"); fi
mkdir -p "$work/fixtures"
classpath=$(native "$framework")
package_id=127
for part in base core entry; do
  mkdir -p "$work/$part/classes" "$work/$part/dex"
  (cd "$here/$part" && "$aapt2" compile --dir res -o "$(native "$work/$part/res.zip")")
  "$aapt2" link -o "$(native "$work/fixtures/$part.apk")" -I "$(native "$framework_res")" \
    --manifest "$(native "$here/$part/AndroidManifest.xml")" --package-id "$package_id" \
    "$(native "$work/$part/res.zip")"
  find "$here/$part/java" -name '*.java' > "$work/sources.txt"
  native_list "$work/sources.txt"
  javac -source 17 -target 17 "${system[@]}" -encoding UTF-8 -nowarn -Xlint:-options \
    -XDstringConcat=inline -cp "$classpath" -d "$(native "$work/$part/classes")" \
    @"$(native "$work/sources.txt")"
  find "$work/$part/classes" -name '*.class' > "$work/dex.txt"
  native_list "$work/dex.txt"
  java -cp "$(native "$r8")" com.android.tools.r8.D8 --release --min-api 26 \
    --output "$(native "$work/$part/dex")" @"$(native "$work/dex.txt")"
  (cd "$work/$part/dex" && jar -u -M -f "$(native "$work/fixtures/$part.apk")" classes.dex)
  classpath+="$separator$(native "$work/$part/classes")"
  ((package_id+=1))
done
# Required split types are an install-session contract, beyond manifest-name
# dependencies. Keep valid and absent-type companions available to the test.
sed 's/android:isolatedSplits="true"/android:isolatedSplits="true" android:isSplitRequired="true" android:requiredSplitTypes="requiredcore"/' \
  "$here/base/AndroidManifest.xml" > "$work/base-required.xml"
"$aapt2" link -o "$(native "$work/fixtures/base-required.apk")" -I "$(native "$framework_res")" \
  --manifest "$(native "$work/base-required.xml")" --package-id 127 "$(native "$work/base/res.zip")"
(cd "$work/base/dex" && jar -u -M -f "$(native "$work/fixtures/base-required.apk")" classes.dex)
sed 's/android:isFeatureSplit="true"/android:isFeatureSplit="true" android:splitTypes="requiredcore"/' \
  "$here/core/AndroidManifest.xml" > "$work/core-typed.xml"
"$aapt2" link -o "$(native "$work/fixtures/core-typed.apk")" -I "$(native "$framework_res")" \
  --manifest "$(native "$work/core-typed.xml")" --package-id 128 "$(native "$work/core/res.zip")"
(cd "$work/core/dex" && jar -u -M -f "$(native "$work/fixtures/core-typed.apk")" classes.dex)
# A valid same-version replacement for an already installed feature. Changing
# only this split beside an unchanged base must still invalidate the source cache.
sed 's/android:versionCode="1"/android:versionCode="1" android:revisionCode="7"/' \
  "$here/entry/AndroidManifest.xml" > "$work/entry-revised.xml"
"$aapt2" link -o "$(native "$work/fixtures/entry-revised.apk")" -I "$(native "$framework_res")" \
  --manifest "$(native "$work/entry-revised.xml")" --package-id 129 "$(native "$work/entry/res.zip")"
(cd "$work/entry/dex" && jar -u -M -f "$(native "$work/fixtures/entry-revised.apk")" classes.dex)
# Same low version, different long version: a cluster-lite-only comparison misses this.
sed 's/android:versionCode="1"/android:versionCode="1" android:versionCodeMajor="1"/' \
  "$here/core/AndroidManifest.xml" > "$work/core-major.xml"
"$aapt2" link -o "$(native "$work/fixtures/core-major.apk")" -I "$(native "$framework_res")" \
  --manifest "$(native "$work/core-major.xml")" --package-id 128 "$(native "$work/core/res.zip")"
"$aapt2" link -o "$(native "$work/fixtures/foreign.apk")" -I "$(native "$framework_res")" \
  --manifest "$(native "$here/core/AndroidManifest.xml")" --rename-manifest-package com.gesher.foreignsplit \
  --package-id 128 "$(native "$work/core/res.zip")"
# jar -u may move a stored resources.arsc off its required four-byte boundary.
# Keep every fixture valid for the same resource-table checks as installed APKs.
for apk in "$work/fixtures/"*.apk; do
  "$zipalign" -f -p 4 "$(native "$apk")" "$(native "$work/aligned.apk")"
  mv "$work/aligned.apk" "$apk"
done
# The real PackageInstaller session path requires a verified signer. A separate
# signed incomplete base reaches required-split validation and its status callback.
keytool -genkeypair -keystore "$(native "$work/key.p12")" -storetype PKCS12 \
  -storepass fixture-only -alias fixture -keyalg RSA -keysize 2048 -validity 36500 \
  -dname 'CN=Gesher split package fixture' > /dev/null 2>&1
java -jar "$(native "$BUILD_TOOLS_DIR/lib/apksigner.jar")" sign \
  --ks "$(native "$work/key.p12")" --ks-pass pass:fixture-only \
  --out "$(native "$work/fixtures/base-required-signed.apk")" \
  "$(native "$work/fixtures/base-required.apk")"
(cd "$work/fixtures" && jar -c -M -f "$(native "$out/split-package-fixtures.zip")" .)
echo "split-package-fixtures.zip written to $out"
