#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
if [[ $# -ne 2 ]]; then
    echo "usage: $0 ANDROID_SDK_ROOT OUTPUT_DIR" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
platform=$1/platforms/android-36/android.jar
export BUILD_TOOLS_DIR=${BUILD_TOOLS_DIR:-$1/build-tools/36.0.0}
APP_DIR=$here SIGN_TEST_APK=1 exec bash "$here/../lifecycle/build.sh" "$2" \
    --framework-res "$platform" --framework-classes "$platform"
