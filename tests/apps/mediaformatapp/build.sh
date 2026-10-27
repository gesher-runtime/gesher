#!/usr/bin/env bash
# Build one dex and two APKs with a single native ABI each (media4, ADR 0064).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
APP_DIR=$here bash "$here/../lifecycle/build.sh" "$@"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
import zipfile
out = Path(sys.argv[1])
base = out / 'mediaformatapp.apk'
native = out / 'media/ndk-format'
with zipfile.ZipFile(base) as archive:
    (native / 'mediaformat-tests.dex').write_bytes(archive.read('classes.dex'))
for abi, name in [('x86_64', 'mediaformatapp-x86_64.apk'), ('arm64-v8a', 'mediaformatapp-arm64.apk')]:
    with zipfile.ZipFile(base) as source, zipfile.ZipFile(out / name, 'w') as target:
        for item in source.infolist():
            target.writestr(item, source.read(item.filename))
        for path in sorted((native / abi).glob('*.so')):
            target.write(path, 'lib/' + abi + '/' + path.name, compress_type=zipfile.ZIP_STORED)
base.unlink()
PY
zipalign=${BUILD_TOOLS_DIR:?BUILD_TOOLS_DIR names the build-tools with zipalign}/zipalign
for name in mediaformatapp-x86_64 mediaformatapp-arm64; do
  "$zipalign" -f -p 4 "$out/$name.apk" "$out/$name.aligned.apk"
  mv "$out/$name.aligned.apk" "$out/$name.apk"
  echo "$name.apk: one native ABI added"
done
