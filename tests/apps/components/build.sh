#!/usr/bin/env bash
# Build components.apk (tests/apps/components) with the lifecycle app's build script.
#
#   tests/apps/components/build.sh <out-dir> --framework-res <framework-res.apk> \
#       --framework-classes <dir|jar> [--core-classes <dir>]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$here exec bash "$here/../lifecycle/build.sh" "$@"
