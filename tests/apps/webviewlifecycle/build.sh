#!/usr/bin/env bash
# Ordinary synthetic headless APK: no network/accounts and no displayed WebView.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$here exec bash "$here/../lifecycle/build.sh" "$@"
