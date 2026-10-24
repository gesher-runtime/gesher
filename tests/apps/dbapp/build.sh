#!/usr/bin/env bash
# Build dbapp.apk (tests/apps/dbapp) with the lifecycle app's build script.
#
#   tests/apps/dbapp/build.sh <out-dir> --framework-res <framework-res.apk> \
#       --framework-classes <dir|jar> [--core-classes <dir>]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$here exec bash "$here/../lifecycle/build.sh" "$@"
