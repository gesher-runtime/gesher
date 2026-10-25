#!/usr/bin/env bash
# The networking libraries of the boot class path (docs/network.md, ADR 0037):
# their sources and generated sources, for tools/build-boot-jars.sh.
#
#   tools/net/net-java.sh <aosp dir> <work dir>
#
# Writes <work dir>/net.txt, the .java files of
#   conscrypt     external/conscrypt: Android.bp's `conscrypt` (the platform's
#                 com.android.org.conscrypt, repackaged/common and platform,
#                 android.net.ssl's public API), with its generated sources:
#                 NativeConstants (constants/src/gen/cpp/generate_constants.cc,
#                 built for this host against BoringSSL's headers),
#                 StaticBlocklist (generate_blocklist.awk), its aconfig flags
#                 (release BP4A's values, as the framework's) and Gesher's
#                 ConscryptStatsLog stand-in (compat/net/java: no statsd);
#   okhttp        external/okhttp: Android.bp's `okhttp` (com.android.okhttp,
#                 which java.net.URL uses for http and https);
#   bouncycastle  external/bouncycastle: Android.bp's `bouncycastle`
#                 (com.android.org.bouncycastle, the BC provider);
# and the generated sources under <work dir>/gen-net. build-boot-jars.sh
# compiles them with libcore and dexes them into core-libart.jar.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
aosp=$1
work=$2
py=${PYTHON:-python3}
release=bp4a  # as tools/build-framework-res.sh: android-16.0.0_r4 is BP4A
cs=$aosp/external/conscrypt
gen=$work/gen-net
rm -rf "$gen" && mkdir -p "$gen/com/android/org/conscrypt"

# NativeConstants.java: BoringSSL's constants, printed by a host program.
cxx=${CXX_FOR_HOST:-c++}
"$cxx" -std=c++17 -o "$work/conscrypt_generate_constants" \
  -I "$aosp/external/boringssl/src/include" "$cs/constants/src/gen/cpp/generate_constants.cc"
"$work/conscrypt_generate_constants" com.android.org.conscrypt \
  > "$gen/com/android/org/conscrypt/NativeConstants.java"

# StaticBlocklist.java, as Android.bp's conscrypt_generated_blocklist.
awk -v package=com.android.org.conscrypt -f "$cs/constants/src/gen/java/generate_blocklist.awk" \
  "$cs/constants/src/gen/java/cert_verify_proc_blocklist.inc" \
  > "$gen/com/android/org/conscrypt/StaticBlocklist.java"

# Conscrypt's aconfig flags, with the release's values.
"$py" "$root/tools/gen/aconfig_values.py" "$aosp/build/release" "$release" > "$work/net.values"
"$py" "$root/tools/gen/aconfig_java.py" "$work/net.values" "$gen" \
  "$cs/conscrypt.aconfig" "$cs/networksecurity.aconfig"

{
  find "$cs/repackaged/common/src/main/java" "$cs/repackaged/platform/src/main/java" \
       "$cs/publicapi/src/main/java" "$gen" "$root/compat/net/java" -name '*.java'
  find "$aosp/external/okhttp/repackaged/android/src/main/java" \
       "$aosp/external/okhttp/repackaged/okhttp/src/main/java" \
       "$aosp/external/okhttp/repackaged/okhttp-urlconnection/src/main/java" \
       "$aosp/external/okhttp/repackaged/okhttp-android-support/src/main/java" \
       "$aosp/external/okhttp/repackaged/okio/okio/src/main/java" -name '*.java'
  find "$aosp/external/bouncycastle/repackaged/bcprov/src/main/java" -name '*.java'
} | sort > "$work/net.txt"
echo "networking libraries: $(wc -l < "$work/net.txt") source files"
