#!/usr/bin/env bash
# Build localeapp.apk (tests/apps/localeapp) with the lifecycle app's build script.
#
#   tests/apps/localeapp/build.sh <out-dir> --framework-res <framework-res.apk> \
#       --framework-classes <dir|jar> [--core-classes <dir>]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$here exec bash "$here/../lifecycle/build.sh" "$@"
