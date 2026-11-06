#!/usr/bin/env bash
# Resource-only density split fixtures for the shell icon reader; never installed.
set -euo pipefail
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
[[ $# == 2 && $1 == --framework-res ]] || { echo 'Pass OUT --framework-res APK' >&2; exit 2; }
framework=$2
repo=$(cd "$(dirname "$0")/../../.." && pwd)
tools=${BUILD_TOOLS_DIR:?Pass cached Android build tools}
native() { echo "$1"; }
separator=:
exe=
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
  separator=';'
  exe=.exe
fi
"$tools/aapt2$exe" compile --dir "$repo/tests/apps/lifecycle/res" -o "$(native "$out/resources.zip")"
# Keep an unsplit input on every host; the second invocation supplies only the
# SDK-generated split manifest, regardless of its density-partition behavior.
"$tools/aapt2$exe" link -o "$(native "$out/icon-full.apk")" \
  -I "$(native "$framework")" --manifest "$(native "$repo/tests/apps/lifecycle/AndroidManifest.xml")" \
  --min-sdk-version 36 --target-sdk-version 36 "$(native "$out/resources.zip")"
"$tools/aapt2$exe" link -o "$(native "$out/icon-base.apk")" \
  -I "$(native "$framework")" --manifest "$(native "$repo/tests/apps/lifecycle/AndroidManifest.xml")" \
  --min-sdk-version 36 --target-sdk-version 36 \
  --split "$(native "$out/icon-density.apk")${separator}xxxhdpi" "$(native "$out/resources.zip")"
python=${PYTHON:-$(command -v python3 || command -v python)}
"$python" "$repo/tests/apps/iconbundle/split_resources.py" "$out"
"$tools/zipalign$exe" -f -p 4 "$(native "$out/icon-base-unaligned.apk")" "$(native "$out/icon-base.apk")"
"$tools/zipalign$exe" -f -p 4 "$(native "$out/icon-density-unaligned.apk")" "$(native "$out/icon-density.apk")"
"$python" - "$out" <<'PY'
from pathlib import Path
import sys,zipfile
out=Path(sys.argv[1])
for name in ['icon-bundle.apks','icon-bundle.xapk']:
    with zipfile.ZipFile(out/name,'w',compression=zipfile.ZIP_DEFLATED) as z:
        z.write(out/'icon-density.apk','splits/config.xxxhdpi.apk')
        z.write(out/'icon-base.apk','base.apk')
print('Shell icon density split/APKS/XAPK fixtures written')
PY
