#!/usr/bin/env bash
# Manual AccountManager / genuine Google token verification client.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$here exec bash "$here/../lifecycle/build.sh" "$@"
