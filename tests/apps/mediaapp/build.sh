#!/usr/bin/env bash
# Build mediaapp.apk (tests/apps/mediaapp) with the lifecycle app's build script,
# then add its assets/ uncompressed (AssetManager.openFd needs them stored) and
# align it again. The media files come from gen-media.sh (run first).
#
#   tests/apps/mediaapp/build.sh <out-dir> --framework-res <framework-res.apk> \
#       --framework-classes <dir|jar> [--core-classes <dir>]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
APP_DIR=$here bash "$here/../lifecycle/build.sh" "$@"
if [[ -d $here/assets ]]; then
  # assets/ and lib/x86_64 (the NDK test library, when built), stored.
  python3 - "$out/mediaapp.apk" "$here" <<'PY'
import os, sys, zipfile
apk, here = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, "a") as z:
    for sub in ("assets", "lib/x86_64"):
        d = os.path.join(here, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            z.write(os.path.join(d, name), sub + "/" + name, compress_type=zipfile.ZIP_STORED)
PY
  zipalign=${BUILD_TOOLS_DIR:?BUILD_TOOLS_DIR names the build-tools with zipalign}/zipalign
  "$zipalign" -f -p 4 "$out/mediaapp.apk" "$out/mediaapp.aligned.apk"
  mv "$out/mediaapp.aligned.apk" "$out/mediaapp.apk"
  echo "mediaapp.apk: assets added ($(ls "$here/assets" | wc -l) files)"
fi
