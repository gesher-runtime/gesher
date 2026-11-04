#!/usr/bin/env bash
# Regenerates SigningTest's APKs (tests/dex-services/fixtures) from AndroidManifest.xml,
# with two new throwaway keys (not kept): unsigned.apk; v2only.apk, signed with the old
# key in a v2 block only; rotated.apk, signed as WhatsApp is: the old key in its v2 and
# v3 blocks (SDK 24-32), the new one in a v3.1 block (SDK 33 and up) with a lineage from
# the old. Prints the keys' certificate SHA-256 digests, which SigningTest names.
#
#   generate.sh <build-tools dir> <framework-res.apk>
#
# Needs build-tools' aapt2, zipalign and lib/apksigner.jar, and a JDK's keytool and java.
set -euo pipefail

bt=$1 framework_res=$2
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pass=pass:fixture-only
apksigner() { java -jar "$bt/lib/apksigner.jar" "$@"; }

"$bt/aapt2" link -o "$work/linked.apk" --manifest "$here/AndroidManifest.xml" -I "$framework_res"
"$bt/zipalign" -f 4 "$work/linked.apk" "$here/unsigned.apk"
for key in old new; do
  keytool -genkeypair -keystore "$work/$key.p12" -storetype PKCS12 -storepass fixture-only \
    -alias "$key" -keyalg RSA -keysize 2048 -validity 36500 \
    -dname "CN=Gesher $key signer, O=Gesher tests" > /dev/null
done
apksigner rotate --out "$work/lineage" --old-signer --ks "$work/old.p12" --ks-pass $pass \
  --new-signer --ks "$work/new.p12" --ks-pass $pass
apksigner sign --ks "$work/old.p12" --ks-pass $pass --next-signer --ks "$work/new.p12" \
  --ks-pass $pass --lineage "$work/lineage" --out "$here/rotated.apk" "$here/unsigned.apk"
apksigner sign --ks "$work/old.p12" --ks-pass $pass --v1-signing-enabled false \
  --v3-signing-enabled false --out "$here/v2only.apk" "$here/unsigned.apk"
rm -f "$here"/*.idsig
apksigner verify --print-certs "$here/rotated.apk" | grep 'certificate SHA-256'
