"""CI-only typed whole-host inventory, kept outside production artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/web3'))
import host_roles

SOURCES = (
    '.github/workflows/aot3-after-resource-component.yml', '.github/workflows/framework.yml',
    'tools/aot3/host_payload.py', 'tools/aot3/preflight.py',
    'tools/build-framework-res.sh', 'tests/apps/sharedlibraryapp/build.sh',
    'tests/dex-graphics/gesher/Aot3SharedLibraryTest.java',
    'tests/shared-libraries/after-res/values/config.xml',
    'tests/windows/run-aot3-resource-component.ps1', 'tests/windows/tracks/73-aot3-shared-libraries.ps1',
    'tests/windows/run-tests.ps1',
    'tests/windows/tracks/61-aot2.ps1', 'tests/windows/tracks/63-aot3-cloud.ps1',
    'patches/art/0730-aot3-validate-after-shared-library-context.patch',
)
REQUIRED = {'dalvikvm.exe', 'dex2oat.exe', 'profman.exe', 'dexdump.exe', 'oatdump.exe',
            'gesher-exec.exe', 'web3-production-role.json', 'core-oj.jar',
            'core-libart.jar', 'core-icu4j.jar', 'hello.dex'}

def inventory(root):
    found = {}
    folded = set()
    for path in sorted(root.rglob('*')):
        name = path.relative_to(root).as_posix()
        assert not path.is_symlink() and name.casefold() not in folded, name
        folded.add(name.casefold())
        if path.is_file():
            found[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        else:
            assert path.is_dir(), name
            found[name] = 'directory'
    assert REQUIRED <= found.keys(), ('Missing actual AOT host tool', REQUIRED - found.keys())
    assert 'binder_sg_actor.exe' not in found and 'binder_sg_wire_peer.exe' not in found
    return found

def require_manifest(expected, actual):
    assert expected == actual, 'Whole host/source/compiler inventory mismatch'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=('capture', 'verify', 'controls'))
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    context = host_roles.context()
    sources = {name: host_roles.sha(ROOT / name) for name in SOURCES}
    files = inventory(args.root)
    role = json.loads((args.root / 'web3-production-role.json').read_text())
    assert all(role.get(key) == value for key, value in context.items())
    assert role['role'] == 'production' and role['test_hook_compiled'] is False
    assert files['dalvikvm.exe'] == role['executable_sha256']
    assert role['compiler']['configured_driver'] and role['compiler']['sha256'] and role['compiler']['metadata_sha256']
    assert role['shared_sources'] == host_roles.sources()
    wanted = dict(context=context, kind='aot3-current-whole-host', sources=sources,
                  members=files, production_role=role)
    if args.mode == 'capture':
        assert not args.manifest.exists()
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(wanted, sort_keys=True) + '\n', encoding='utf-8')
    else:
        actual_manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
        require_manifest(actual_manifest, wanted)
        if args.mode == 'controls':
            root = args.root.resolve()
            assert root.is_relative_to(ROOT.resolve()), 'Controls require an owned checkout payload'
            saved = root.parent / ('.held-aot-oatdump-' + uuid.uuid4().hex)
            assert not saved.exists()
            tool = root / 'oatdump.exe'
            tool.rename(saved)
            rejected = False
            try:
                inventory(root)
            except AssertionError as error:
                assert 'Missing actual AOT host tool' in str(error), error
                rejected = True
            finally:
                saved.rename(tool)
            assert rejected, 'Missing actual tool was accepted'
            extra = root / ('.extra-aot-member-' + uuid.uuid4().hex)
            with extra.open('xb') as stream:
                stream.write(b'extra')
            rejected = False
            try:
                require_manifest(actual_manifest, dict(wanted, members=inventory(root)))
            except AssertionError as error:
                assert 'Whole host/source/compiler inventory mismatch' in str(error), error
                rejected = True
            finally:
                extra.unlink()
            assert rejected, 'Extra actual member was accepted'
            require_manifest(actual_manifest, dict(wanted, members=inventory(root)))
            print('ACTUAL_AOT_HOST_NEGATIVES missing/extra rejected; original complete host restored', flush=True)
        print('ACTUAL_AOT_CURRENT_HOST ' + json.dumps(dict(context=context, kind=wanted['kind'],
              members=len(files), sources=sources, compiler=role['compiler'],
              tools={name: files[name] for name in sorted(REQUIRED)}), sort_keys=True), flush=True)

if __name__ == '__main__':
    main()
