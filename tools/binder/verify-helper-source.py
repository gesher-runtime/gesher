"""CI-only: materialize and check the exact independently reviewed helper bytes."""
import hashlib
import os
from pathlib import Path
import subprocess

assert os.environ.get('GITHUB_ACTIONS') == 'true', 'CI only'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == os.environ['GITHUB_SHA']
pins = {
    'download-verified-artifact.py': ('c12018f78db23e7696298abc864ce1c4d9d788e8', 'd82ec81f14f879958eeae2489acecc2ec39cc903dc1d564b376ab5b829320b6e'),
    'assemble-artifacts.py': ('8d01e5084c8b0aac7f06bc7eae75cf14e431f5b2', 'ca9ddb18ac2fae48201c66f8279b4631868cea401236e0e5e663b7fd1e09f79c'),
    'test-verified-artifact.py': ('a97840fd9760ed65f8b317621266e67e8092f10f', '6fcb0c6e96887f56af700e67f8aee0950dc5fd0f4c2bc6709ed6b3c2805d0448'),
}
for name, (blob, digest) in pins.items():
    path = Path('tools/surface') / name
    actual = subprocess.check_output(['git', 'rev-parse', 'HEAD:' + path.as_posix()], text=True).strip()
    assert actual == blob, (name, 'unreviewed Git blob')
    data = subprocess.check_output(['git', 'cat-file', 'blob', blob])
    assert hashlib.sha256(data).hexdigest() == digest, (name, 'unreviewed source bytes')
    # Windows checkout newline conversion must not weaken byte identity.
    path.write_bytes(data)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    print(f'ACTUAL_BINDER_STRICT_HELPER name={name} blob={blob} sha256={digest}', flush=True)
