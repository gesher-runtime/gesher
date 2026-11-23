"""Seal and verify CI Binder mode fixtures against their actual producer attempt."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/web3'))
sys.path.insert(0, str(ROOT / 'tools/ci'))
import host_roles
import producer_context

ROLE = 'binder-ordinary-modes'
ABIS = ('x86_64', 'arm64-v8a')
LIBRARIES = ('libbindersg.so', 'libc++_shared.so')
MEMBERS = frozenset({'binder-sg.dex'} | {
    f'{abi}/{name}' for abi in ABIS for name in ('binder-sg-actor', *LIBRARIES)
} | {f'binder-sg-{role}-{abi}.apk' for abi in ABIS for role in ('server', 'client')})


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        require(key not in value, 'Duplicate Binder fixture manifest key')
        value[key] = item
    return value


def invalid_constant(_):
    raise RuntimeError('Invalid Binder fixture JSON constant')


def payload(root):
    root = Path(root).absolute()
    for path in (root, *root.parents, *root.rglob('*')):
        require(not path.is_symlink() and not (hasattr(path, 'is_junction') and path.is_junction()),
                'Linked Binder fixture path')
    files = {path.relative_to(root).as_posix() for path in root.rglob('*') if path.is_file()}
    require(files - {'source.json'} == MEMBERS, 'Binder fixture member set mismatch')
    return {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in sorted(MEMBERS)}


def seal(root):
    """Record the real producer identity after checking APK/library/dex equality."""
    info = host_roles.context()
    files = payload(root)
    for abi in ABIS:
        for role in ('server', 'client'):
            with zipfile.ZipFile(root / f'binder-sg-{role}-{abi}.apk') as apk:
                for name in LIBRARIES:
                    require(apk.read(f'lib/{abi}/{name}') == (root / abi / name).read_bytes(),
                            'Binder APK/native-library bytes differ')
                require(apk.read('classes.dex') == (root / 'binder-sg.dex').read_bytes(),
                        'Binder APK/dex bytes differ')
    manifest = dict(info, role=ROLE, files=files)
    (root / 'source.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print('SG mode fixtures built with exact APK/native-library/dex identity; runtime still pending')


def verify(root, source):
    consumer = host_roles.context()
    require(source == consumer['source'], 'Binder requested source differs from the exact checkout')
    path = root / 'source.json'
    raw = path.read_bytes()
    manifest = json.loads(raw, object_pairs_hook=unique_object, parse_constant=invalid_constant)
    require(isinstance(manifest, dict) and set(manifest) == set(producer_context.CONTEXT_KEYS) | {'role', 'files'},
            'Binder fixture manifest fields mismatch')
    files = manifest['files']
    require(isinstance(files, dict) and set(files) == MEMBERS and all(
        isinstance(digest, str) and re.fullmatch(r'[0-9a-f]{64}', digest) for digest in files.values()),
        'Binder fixture file/hash policy mismatch')
    # An older attempt is admitted only by the historical run/job, retained
    # current execution and exact digest-checked archive proof. The consumer's
    # GITHUB_RUN_ATTEMPT remains its real attempt throughout.
    producer = producer_context.authenticate(path, consumer, ROLE)
    require(payload(root) == files, 'SG consumed fixture hash mismatch')
    require(path.read_bytes() == raw, 'Binder fixture manifest changed during verification')
    for name, digest in sorted(files.items()):
        print(f'ACTUAL_SG_FIXTURE member={name} sha256={digest} source={producer["source"]} '
              f'run={producer["run"]} producer_attempt={producer["attempt"]} '
              f'consumer_attempt={consumer["attempt"]}', flush=True)
    return producer


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('seal', 'verify'))
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--source')
    args = parser.parse_args(argv)
    if args.mode == 'verify':
        if not args.source:
            parser.error('verify requires --source')
        verify(args.fixtures, args.source)
    else:
        if args.source:
            parser.error('seal records the actual checkout, not --source')
        seal(args.fixtures)


if __name__ == '__main__':
    main()
