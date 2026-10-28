"""Run CI-only adversarial checks against the actual artifact extractor."""
import hashlib
import importlib.util
import os
from pathlib import Path
import tempfile
import zipfile

assert os.environ.get('GITHUB_ACTIONS') == 'true'
spec = importlib.util.spec_from_file_location('surface_artifact', Path(__file__).with_name('download-verified-artifact.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def fixture(root, name, entries):
    path = root / (name + '.zip')
    with zipfile.ZipFile(path, 'w') as archive:
        for entry, content in entries:
            archive.writestr(entry, content)
    data = path.read_bytes()
    return path, hashlib.sha256(data), 'sha256:' + hashlib.sha256(data).hexdigest()

def reject(fn, label):
    try:
        fn()
    except (RuntimeError, OSError, zipfile.BadZipFile):
        print('SURFACE_ARCHIVE_NEGATIVE ' + label + '=rejected', flush=True)
    else:
        raise AssertionError(label + ' accepted')

with tempfile.TemporaryDirectory(prefix='surface-archive-negative-') as temporary:
    root = Path(temporary)
    archive, digest, good = fixture(root, 'valid', [('nested/value.txt', 'actual')])
    reject(lambda: module.extract_verified(archive, 'sha256:' + '0' * 64, root/'wrong', digest), 'wrong-digest')
    assert not (root/'wrong').exists()
    module.extract_verified(archive, good, root/'valid', digest)
    assert (root/'valid/nested/value.txt').read_text() == 'actual'
    reject(lambda: module.extract_verified(archive, good, root/'valid', digest), 'extra-existing-member')
    for label, entries in [('traversal',[('../escape','x')]), ('duplicate',[('A.txt','x'),('a.txt','y')]), ('windows-alias',[('file.txt.','x')])]:
        archive, digest, good = fixture(root, label, entries)
        reject(lambda: module.extract_verified(archive, good, root/label, digest), label)
        assert not (root/label).exists()
    reject(lambda: module.extract_verified(root/'absent.zip', good, root/'missing', digest), 'missing-archive')
    assert not (root/'escape').exists()
print('SURFACE_ARCHIVE_NEGATIVES complete=6', flush=True)
