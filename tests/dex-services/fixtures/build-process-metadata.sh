#!/usr/bin/env bash
# Reproduce the committed code-free manifest fixture; no APK code is executed or signed.
# Usage: build-process-metadata.sh AAPT2 FRAMEWORK_RES_APK
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
"$1" link --manifest "$here/process-metadata.xml" -I "$2" -o "$here/process-metadata.apk"
