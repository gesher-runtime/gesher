#!/usr/bin/env bash
# Build recordapp.apk (tests/apps/recordapp; media3, ADR 0059) with the lifecycle app's
# build script, then add lib/x86_64 (librecord_ndk_test.so, the NDK input test library,
# when built: .github/workflows/media.yml) stored, and align it again.
#
#   tests/apps/recordapp/build.sh <out-dir> --framework-res <framework-res.apk> \
#       --framework-classes <dir|jar> [--core-classes <dir>]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
APP_DIR=$here bash "$here/../lifecycle/build.sh" "$@"
if [[ -d $here/lib/x86_64 ]]; then
  python3 - "$out/recordapp.apk" "$here" <<'PY'
import os, sys, zipfile
apk, here = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, "a") as z:
    d = os.path.join(here, "lib/x86_64")
    for name in sorted(os.listdir(d)):
        z.write(os.path.join(d, name), "lib/x86_64/" + name, compress_type=zipfile.ZIP_STORED)
PY
  zipalign=${BUILD_TOOLS_DIR:?BUILD_TOOLS_DIR names the build-tools with zipalign}/zipalign
  "$zipalign" -f -p 4 "$out/recordapp.apk" "$out/recordapp.aligned.apk"
  mv "$out/recordapp.aligned.apk" "$out/recordapp.apk"
  echo "recordapp.apk: lib/x86_64 added"
fi
