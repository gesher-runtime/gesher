"""CI-only ordinary host actor payload, bound to the sealed production role."""
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path('tools/web3').resolve()))
import host_roles

root = Path('ci-suppliers/binder2_sg_host')
manifest = json.loads((root / 'binder-host-source.json').read_text())
context = host_roles.context()
assert all(manifest[k] == v for k, v in context.items()), 'Actor source/run/attempt/repository mismatch'
assert manifest['role'] == 'binder-ordinary-actor'
production = json.loads(Path('dist/web3-production-role.json').read_text())
assert production['role'] == 'production' and production['test_hook_compiled'] is False
assert manifest['compiler'] == production['compiler']
assert manifest['production_runtime_sha256'] == production['executable_sha256']
assert set(manifest['files']) == {'binder_sg_actor.exe', 'binder-sg-runtime-source.txt'}
assert {p.name for p in root.iterdir()} == set(manifest['files']) | {'binder-host-source.json'}
for name, digest in manifest['files'].items():
    assert host_roles.sha(root / name) == digest, ('Actor payload hash mismatch', name)
assert (root / 'binder-sg-runtime-source.txt').read_text().strip() == context['source']
assert set(manifest['sources']) == {
    'compat/posix/src/binder.cc', 'compat/posix/src/binder_sg.h', 'compat/posix/src/binder_wire.h',
    'compat/binder/binderd.cc', 'tests/binder/sg-actor.h', 'tests/binder/run-modes.ps1', 'tests/binder/OutputPageObserver.cs'}
for path, digest in manifest['sources'].items():
    assert host_roles.sha(path) == digest, ('Actor/current source mismatch', path)
print('ACTUAL_BINDER_FULL_HOST ' + json.dumps(manifest, sort_keys=True), flush=True)
