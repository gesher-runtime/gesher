"""Shared authenticated publisher archive, fresh compiler source, complete tracked source."""
from contextlib import contextmanager
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def source_inventory():
    # The complete source tree is bound even in consumers' deliberate sparse checkouts.
    rows = subprocess.check_output(['git', 'ls-tree', '-r', '-z', 'HEAD'], cwd=ROOT).split(b'\0')
    sparse = {row[2:].decode() for row in subprocess.check_output(['git', 'ls-files', '-v', '-z'], cwd=ROOT).split(b'\0') if row.startswith(b'S ')}
    result = {}
    for row in filter(None, rows):
        meta, name = row.split(b'\t', 1)
        mode, kind, blob = meta.decode().split()
        name = name.decode()
        if kind != 'blob' or mode not in ('100644', '100755'):
            raise RuntimeError('Nonregular tracked source: ' + name)
        path = ROOT / name
        if name not in sparse:
            if not path.is_file() or path.is_symlink():
                raise RuntimeError('Missing or linked source: ' + name)
            actual = subprocess.check_output(['git', 'hash-object', '--', name], cwd=ROOT, text=True).strip()
            if actual != blob:
                raise RuntimeError('Dirty tracked source: ' + name)
        result[name] = {'mode': mode, 'git_blob': blob}
    return result

@contextmanager
def authenticated_archive(path):
    path = path.resolve(strict=True)
    if path.stat().st_size != 663987688:
        raise RuntimeError('Wrong publisher archive size')
    with path.open('rb') as stream:
        sha1 = hashlib.file_digest(stream, 'sha1').hexdigest()
    if sha1 != '090e8083a715fdb1a3e402d0763c388abb03fb4e':
        raise RuntimeError('Wrong publisher archive SHA1')
    with path.open('rb') as stream:
        sha256 = hashlib.file_digest(stream, 'sha256').hexdigest()
    with tempfile.TemporaryDirectory(prefix='combined-authenticated-ndk-') as temp:
        subprocess.run(['unzip', '-q', str(path), '-d', temp], check=True, timeout=180)
        yield Path(temp) / 'android-ndk-r27c', {'publisher_sha1': sha1, 'sha256': sha256, 'size': path.stat().st_size}
