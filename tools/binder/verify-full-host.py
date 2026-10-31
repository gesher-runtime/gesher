"""CI-only ordinary host actor payload, bound to the sealed production role."""
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path('tools/web3').resolve()))
import host_roles

def verify_actor(root, production, context, strict_payload=True):
    manifest = json.loads((root / 'binder-host-source.json').read_text())
    assert all(manifest[k] == v for k, v in context.items()), 'Actor source/run/attempt/repository mismatch'
    assert manifest['role'] == 'binder-ordinary-actor'
    assert production['role'] == 'production' and production['test_hook_compiled'] is False
    assert manifest['compiler'] == production['compiler']
    assert manifest['production_runtime_sha256'] == production['executable_sha256']
    argv = manifest['actor_compile_command']
    assert isinstance(argv, list) and len(argv) > 1
    configured = production['compiler']['configured_driver']
    assert (argv[0] == configured and manifest['actor_launcher'] is None) or (
        argv[1] == configured and manifest['actor_launcher'] == production['compiler']['launcher'] and manifest['actor_launcher'])
    expected = {'binder_sg_actor.exe', 'binder-sg-runtime-source.txt'} | host_roles.selected_members(
        {'binder_sg_actor.exe'}, manifest['compiler'], 'binder-ordinary-actor')
    assert set(manifest['files']) == expected
    if strict_payload:
        assert {p.name for p in root.iterdir()} == expected | {'binder-host-source.json'}
    for name, digest in manifest['files'].items():
        assert host_roles.sha(root / name) == digest, ('Actor payload hash mismatch', name)
    assert (root / 'binder-sg-runtime-source.txt').read_text().strip() == context['source']
    assert set(manifest['sources']) == {
        'compat/posix/src/binder.cc', 'compat/posix/src/binder_sg.h', 'compat/posix/src/binder_wire.h',
        'compat/binder/binderd.cc', 'cmake/aosp/servicemanager.cmake', 'tests/binder/sg-actor.cc',
        'tests/binder/sg-actor.h', 'tests/binder/run-modes.ps1', 'tests/binder/OutputPageObserver.cs'}
    for path, digest in manifest['sources'].items():
        assert host_roles.sha(path) == digest, ('Actor/current source mismatch', path)
    host_roles.verify_selected(root, manifest.get('selected_link'), manifest['compiler'], context,
                               {'binder_sg_actor.exe'}, 'binder-ordinary-actor')
    host_roles.emit_record('ACTUAL_BINDER_FULL_HOST', manifest, flush=True)
    return manifest


if __name__ == '__main__':
    verify_actor(Path('ci-suppliers/binder2_sg_host'), json.loads(Path('dist/web3-production-role.json').read_text()), host_roles.context())
