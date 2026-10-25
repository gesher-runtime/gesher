#!/usr/bin/env bash
# Build netapp.apk (tests/apps/netapp, docs/network.md) with the lifecycle app's build script.
#
#   tests/apps/netapp/build.sh <out-dir> --framework-res <framework-res.apk> \
#       --framework-classes <dir|jar> [--core-classes <dir>]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$here exec bash "$here/../lifecycle/build.sh" "$@"
