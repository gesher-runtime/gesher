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
    for label, entries in [('traversal',[('../escape','x')]), ('duplicate',[('A.txt','x'),('a.txt','y')]), ('windows-alias',[('file.txt.','x')]), ('prefix-collision',[('A','x'),('a/child','y')])]:
        archive, digest, good = fixture(root, label, entries)
        reject(lambda: module.extract_verified(archive, good, root/label, digest), label)
        assert not (root/label).exists()
    reject(lambda: module.extract_verified(root/'absent.zip', good, root/'missing', digest), 'missing-archive')
    reject(lambda: module.HttpsOnly().redirect_request(None, None, 302, '', {}, 'http://example.test/file'), 'insecure-redirect')
    assert not (root/'escape').exists()
    spec2 = importlib.util.spec_from_file_location('surface_assembly', Path(__file__).with_name('assemble-artifacts.py'))
    assembly = importlib.util.module_from_spec(spec2)
    spec2.loader.exec_module(assembly)
    for name in ('baseline', 'refresh', 'unexpected'):
        (root/name).mkdir()
    (root/'baseline/framework.jar').write_text('original')
    (root/'refresh/framework.jar').write_text('new')
    (root/'unexpected/framework.jar').write_text('foreign')
    assembly.assemble(root/'baseline', root/'assembled', root/'owners.json', 'base_framework')
    reject(lambda: assembly.assemble(root/'unexpected', root/'assembled', root/'owners.json', 'base_graphics'), 'unexpected-overlay')
    assert (root/'assembled/framework.jar').read_text() == 'original'
    assembly.assemble(root/'refresh', root/'assembled', root/'owners.json', 'current_framework')
    assert (root/'assembled/framework.jar').read_text() == 'new'
    # Use fresh isolated roots to prove the one explicitly owned baseline replacement.
    (root/'base-graphics').mkdir()
    (root/'graphics').mkdir()
    (root/'base-graphics/skia_probe.exe').write_text('base probe')
    (root/'graphics/skia_probe.exe').write_text('graphics probe')
    assembly.assemble(root/'base-graphics', root/'graphics-out', root/'graphics-owners.json', 'base_windows_x64')
    assembly.assemble(root/'graphics', root/'graphics-out', root/'graphics-owners.json', 'base_graphics')
    assert (root/'graphics-out/skia_probe.exe').read_text() == 'graphics probe'
print('SURFACE_ARCHIVE_NEGATIVES complete=9', flush=True)
