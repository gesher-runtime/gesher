#!/usr/bin/env bash
# Build videoapp.apk (tests/apps/videoapp) with the lifecycle app's build script,
# then add its assets/ uncompressed (AssetManager.openFd needs them stored) and
# align it again. The clips come from gen-media.sh (run first).
#
#   tests/apps/videoapp/build.sh <out-dir> --framework-res <framework-res.apk> \
#       --framework-classes <dir|jar> [--core-classes <dir>]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
APP_DIR=$here bash "$here/../lifecycle/build.sh" "$@"
if [[ -d $here/assets ]]; then
  python3 - "$out/videoapp.apk" "$here/assets" <<'PY'
import os, sys, zipfile
apk, d = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, "a") as z:
    for name in sorted(os.listdir(d)):
        z.write(os.path.join(d, name), "assets/" + name, compress_type=zipfile.ZIP_STORED)
PY
  zipalign=${BUILD_TOOLS_DIR:?BUILD_TOOLS_DIR names the build-tools with zipalign}/zipalign
  "$zipalign" -f -p 4 "$out/videoapp.apk" "$out/videoapp.aligned.apk"
  mv "$out/videoapp.aligned.apk" "$out/videoapp.apk"
  echo "videoapp.apk: assets added ($(ls "$here/assets" | wc -l) files)"
fi
