#!/usr/bin/env bash
# Build a real installed app for microG cross-UID API isolation and runtime-permission tests.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$here exec bash "$here/../lifecycle/build.sh" "$@"
