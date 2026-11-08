#!/usr/bin/env bash
# Local WebView2 JavaScript-worker/HttpOnly-cookie regression; no network downloads with cached tools.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$here exec bash "$here/../lifecycle/build.sh" "$@"
