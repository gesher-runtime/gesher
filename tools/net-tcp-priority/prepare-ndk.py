"""Authenticate the r27c publisher archive on every use, then freshly extract it."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import urllib.request

assert os.environ.get('GITHUB_ACTIONS') == 'true'
url = 'https://dl.google.com/android/repository/android-ndk-r27c-linux.zip'
# Publisher: https://github.com/android/ndk/wiki/Home/24fe2d7ee3591346e0e8ae615977a15c0a4fba40
expected_sha1, expected_size = '090e8083a715fdb1a3e402d0763c388abb03fb4e', 663987688
archive = Path('tcp-ndk-archive/android-ndk-r27c-linux.zip')
archive.parent.mkdir(exist_ok=True)
if not archive.exists():
    with urllib.request.urlopen(url, timeout=120) as response, archive.open('wb') as out:
        while chunk := response.read(1024 * 1024):
            out.write(chunk)
sha1, sha256 = hashlib.sha1(), hashlib.sha256()
with archive.open('rb') as source:
    while chunk := source.read(1024 * 1024):
        sha1.update(chunk); sha256.update(chunk)
assert archive.stat().st_size == expected_size and sha1.hexdigest() == expected_sha1, 'Publisher NDK archive authentication failed'
fresh = Path('tcp-ndk-publisher')
assert not fresh.exists(), 'NDK extraction must be fresh'
subprocess.check_call(['unzip', '-q', str(archive), '-d', str(fresh)])
properties = Path(os.environ['NDK_DIR']) / 'source.properties'
assert 'Pkg.Revision = 27.2.12479018' in properties.read_text()
receipt = dict(url=url, publisher_sha1=expected_sha1, bytes=expected_size, archive_sha256=sha256.hexdigest(), extraction=str(Path(os.environ['NDK_DIR']).resolve(strict=True)), revision='27.2.12479018', properties_sha256=hashlib.sha256(properties.read_bytes()).hexdigest())
Path('tcp-ndk-source.json').write_text(json.dumps(receipt))
print('TCP_PRIORITY_NDK_PUBLISHER', json.dumps(receipt))
