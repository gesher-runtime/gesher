#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
bash "$here/build-targets.sh" "$@"
bash "$here/build-observer.sh" "$@"
