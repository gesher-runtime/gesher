#!/usr/bin/env bash
# Regenerates SigningTest's APKs (tests/dex-services/fixtures) from AndroidManifest.xml,
# with two new throwaway keys (not kept): unsigned.apk; v2only.apk, signed with the old
# key in a v2 block only; rotated.apk, signed as WhatsApp is: the old key in its v2 and
# v3 blocks (SDK 24-32), the new one in a v3.1 block (SDK 33 and up) with a lineage from
# the old. Prints the keys' certificate SHA-256 digests, which SigningTest names.
#
#   generate.sh <build-tools dir> <framework-res.apk>
#
# Needs build-tools' aapt2, zipalign and lib/apksigner.jar, a JDK, and openssl.
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
# Detached, synthetic installer-advisory checksum signature. Its throwaway signer is
# deliberately different from the APK signer; it must never establish APK trust.
printf '\000\000\000\010\000\000\000\040' > "$work/checksums.bin"
openssl dgst -sha256 -binary "$here/v2only.apk" >> "$work/checksums.bin"
openssl req -x509 -newkey rsa:2048 -nodes -days 36500 \
  -subj '/CN=Gesher synthetic checksum signer' -keyout "$work/checksum-key.pem" \
  -out "$work/checksum-cert.pem" > /dev/null 2>&1
openssl cms -sign -binary -nosmimecap -noattr -md sha256 -in "$work/checksums.bin" \
  -signer "$work/checksum-cert.pem" -inkey "$work/checksum-key.pem" -outform DER \
  -out "$here/checksum-signature.der"
apksigner verify --print-certs "$here/rotated.apk" | grep 'certificate SHA-256'
