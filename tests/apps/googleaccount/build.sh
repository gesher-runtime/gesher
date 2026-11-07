#!/usr/bin/env bash
# Manual AccountManager / genuine Google token verification client.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift

# Google identifies Android OAuth clients by package AND their real signing
# certificate. Unlike offline fixtures, this client must never remain unsigned.
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
else
  native() { echo "$1"; }
fi
signer=${APKSIGNER_JAR:-${BUILD_TOOLS_DIR:+$BUILD_TOOLS_DIR/lib/apksigner.jar}}
if [[ -z $signer || ! -f $signer ]]; then
  echo "Set BUILD_TOOLS_DIR or APKSIGNER_JAR to sign the Google account client." >&2
  exit 2
fi
staging=$out/.googleaccount-unsigned
APP_DIR=$here bash "$here/../lifecycle/build.sh" "$staging" "$@"
identity=${GOOGLEACCOUNT_SIGNING_DIR:-$out/.googleaccount-signing}
mkdir -p "$identity"
if [[ ! -f $identity/identity.p12 ]]; then
  if [[ -f $identity/password.txt ]]; then
    echo "Incomplete signing identity; retain/recover the existing key instead of rotating it." >&2
    exit 2
  fi
  (umask 077; od -An -N32 -tx1 /dev/urandom | tr -d ' \r\n' > "$identity/password.txt")
  keytool -genkeypair -noprompt -alias gesher-account-test -keyalg RSA -keysize 3072 \
    -validity 3650 -dname 'CN=Gesher Google Account Test,O=Gesher' -storetype PKCS12 \
    -keystore "$(native "$identity/identity.p12")" \
    -storepass:file "$(native "$identity/password.txt")" \
    -keypass:file "$(native "$identity/password.txt")"
fi
java -jar "$(native "$signer")" sign --ks "$(native "$identity/identity.p12")" \
  --ks-key-alias gesher-account-test --ks-pass "file:$(native "$identity/password.txt")" \
  --out "$(native "$out/googleaccount.signed.apk")" "$(native "$staging/googleaccount.apk")"
java -jar "$(native "$signer")" verify "$(native "$out/googleaccount.signed.apk")"
mv "$out/googleaccount.signed.apk" "$out/googleaccount.apk"
echo "Signed googleaccount.apk with the retained local developer identity."
