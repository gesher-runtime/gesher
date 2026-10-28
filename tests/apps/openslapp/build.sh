#!/usr/bin/env bash
# CI only: identical code in manifest-allowed/denied, single-ABI APKs.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
APP_DIR=$here bash "$here/../lifecycle/build.sh" "$@"
mkdir -p "$work/openslapp-denied"
cp -R "$here/java" "$here/res" "$here/AndroidManifest.xml" "$work/openslapp-denied/"
python3 - "$work/openslapp-denied/AndroidManifest.xml" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
assert text.count('package="com.gesher.opensl"') == 1
text = text.replace('package="com.gesher.opensl"', 'package="com.gesher.opensl.denied"')
permission = '    <uses-permission android:name="android.permission.RECORD_AUDIO" />\n'
assert text.count(permission) == 1
path.write_text(text.replace(permission, ''))
PY
APP_DIR="$work/openslapp-denied" bash "$here/../lifecycle/build.sh" "$@"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
import zipfile
out = Path(sys.argv[1])
native = out / 'media/opensl-contracts'
with zipfile.ZipFile(out / 'openslapp.apk') as archive:
    (native / 'opensl-tests.dex').write_bytes(archive.read('classes.dex'))
for permission in ['', '-denied']:
    base = out / ('openslapp' + permission + '.apk')
    for abi, label in [('x86_64', 'x86_64'), ('arm64-v8a', 'arm64')]:
        name = 'openslapp' + permission + '-' + label + '.apk'
        with zipfile.ZipFile(base) as source, zipfile.ZipFile(out / name, 'w') as target:
            for item in source.infolist():
                target.writestr(item, source.read(item.filename))
            for path in sorted((native / abi).glob('*.so')):
                target.write(path, 'lib/' + abi + '/' + path.name, compress_type=zipfile.ZIP_STORED)
    base.unlink()
PY
zipalign=${BUILD_TOOLS_DIR:?BUILD_TOOLS_DIR names the build-tools with zipalign}/zipalign
for name in openslapp-x86_64 openslapp-arm64 openslapp-denied-x86_64 openslapp-denied-arm64; do
  "$zipalign" -f -p 4 "$out/$name.apk" "$out/$name.aligned.apk"
  mv "$out/$name.aligned.apk" "$out/$name.apk"
  echo "$name.apk: one native ABI and its actual manifest permission"
done
