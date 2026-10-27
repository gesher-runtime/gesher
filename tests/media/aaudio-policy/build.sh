#!/usr/bin/env bash
# CI only: real stream calls with typed API36 extensions on both guest ABIs.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1/media/aaudio-policy" && cd "$1/media/aaudio-policy" && pwd)
ndk=$(cd "$2" && pwd)
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
for abi in x86_64 arm64-v8a; do
  case $abi in
    x86_64) triple=x86_64-linux-android ;;
    arm64-v8a) triple=aarch64-linux-android ;;
  esac
  mkdir -p "$out/$abi"
  "$llvm/bin/${triple}29-clang++" -std=c++20 -O2 -fstack-protector-strong -Wall -Wextra -Werror \
    -shared -fPIC -Wl,-soname,libaaudio_policy.so -o "$out/$abi/libaaudio_policy.so" \
    "$here/aaudio_policy_test.cc" -laaudio -ldl
  "$llvm/bin/${triple}29-clang++" -std=c++20 -O2 -fstack-protector-strong -Wall -Wextra -Werror \
    -DMEDIA4_AAUDIO_ELF -fPIE -pie -Wl,-rpath,'$ORIGIN' -o "$out/$abi/aaudio-policy-test" \
    "$here/aaudio_policy_test.cc" -laaudio -ldl
  cp "$llvm/sysroot/usr/lib/$triple/libc++_shared.so" "$out/$abi/"
  "$llvm/bin/llvm-readelf" -h "$out/$abi/libaaudio_policy.so" | head -20
  "$llvm/bin/llvm-readelf" -h "$out/$abi/aaudio-policy-test" | head -20
done
