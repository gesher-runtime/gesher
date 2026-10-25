#!/usr/bin/env bash
# What Gesher's networking ships besides dalvikvm.exe and core-libart.jar
# (docs/network.md), into a dist directory:
#   cacerts/          Android's CA certificates (Conscrypt's apex/ca-certificates),
#                     which dalvikvm joins with the roots Windows trusts in
#                     $ANDROID_ROOT/etc/security/cacerts (compat/net/trust_store.cc)
#   NET-NOTICES.txt   the licenses of BoringSSL, Conscrypt, OkHttp and BouncyCastle
#
#   tools/net/collect.sh <aosp dir> <dist dir>
set -euo pipefail

aosp=$1
dist=$2
rm -rf "$dist/cacerts"
mkdir -p "$dist/cacerts"
cp "$aosp"/external/conscrypt/apex/ca-certificates/files/* "$dist/cacerts/"
{
  for p in boringssl:NOTICE conscrypt:NOTICE okhttp:LICENSE bouncycastle:NOTICE; do
    echo "===== external/${p%%:*} ($(basename "${p#*:}")) ====="
    echo
    cat "$aosp/external/${p%%:*}/${p#*:}"
    echo
  done
} > "$dist/NET-NOTICES.txt"
echo "cacerts: $(ls "$dist/cacerts" | wc -l) certificates; NET-NOTICES.txt"
