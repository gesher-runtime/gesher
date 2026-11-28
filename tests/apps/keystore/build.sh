#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$here SIGN_TEST_APK=1 exec bash "$here/../lifecycle/build.sh" "$@"
