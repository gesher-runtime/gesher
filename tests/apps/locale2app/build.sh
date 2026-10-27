#!/usr/bin/env bash
# Real public LocaleManager/LocaleConfig fixture, built only in CI.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
APP_DIR=$here exec bash "$here/../lifecycle/build.sh" "$@"
