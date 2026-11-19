#!/usr/bin/env python3
"""Seal default-OFF production capture and separately named synthetic CI hosts."""
import argparse
import fnmatch
import json
from pathlib import Path
import re
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'web3'))
import host_roles as shared

FLAG = 'GESHER_CAPTURE_TEST_SOURCES'
SENTINEL = b'Gesher isolated capture fixture sources enabled'
FILES = {'dalvikvm.exe': 'dalvikvm-media-capture-test.exe',
         'media_probe.exe': 'media_probe-media-capture-test.exe'}
SOURCES = ('compat/media/audio/capture.cc', 'compat/media/audio/capture.h',
           'compat/media/audio/capture_wasapi.cc', 'cmake/aosp/media.cmake')


def source_hashes():
    return {name: shared.sha(shared.ROOT / name) for name in SOURCES}


def production(build, dist, state):
    shared.require(shared.cache(build).get(FLAG) == 'OFF', 'Production capture must be explicit OFF')
    shared.require(shared.cache(build).get('GESHER_WEB3_QUERY_TESTS') == 'OFF', 'Other fixtures must be OFF')
    installer = (shared.ROOT / 'tools/package/gesher.iss').read_text(encoding='utf-8')
    entries = re.findall(r'^\s*Source:\s*"([^"]+)";', installer, flags=re.MULTILINE)
    shared.require(entries, 'Missing installer source inventory')
    for name in FILES.values():
        for pattern in entries:
            shared.require(not fnmatch.fnmatchcase(('{#BuildDir}/' + name).casefold(),
                           pattern.replace('\\', '/').casefold()), 'Installer includes capture fixture')
    role = dict(shared.context(), role='capture-production', fixture=False,
                compiler=shared.compiler(build), sources=source_hashes(), files={})
    for name in FILES:
        path = dist / name
        shared.require(SENTINEL not in path.read_bytes(), 'Synthetic source leaked into production: ' + name)
        role['files'][name] = shared.sha(path)
    (dist / 'media-capture-production-role.json').write_text(json.dumps(role, indent=2) + '\n')
    state.write_text(json.dumps(dict(role=role, commands=[(key, value) for key, value
                          in shared.commands(build).items()])) + '\n')


def fixture(build, dist, state, output):
    shared.require(shared.cache(build).get(FLAG) == 'ON', 'Capture fixture must be explicit ON')
    shared.require(shared.cache(build).get('GESHER_WEB3_QUERY_TESTS') == 'OFF', 'No mixed fixture roles')
    before = json.loads(state.read_text())
    role = before['role']
    shared.require(all(role[k] == v for k, v in shared.context().items()), 'Fixture source/run mismatch')
    shared.require(role['compiler'] == shared.compiler(build), 'Fixture compiler mismatch')
    shared.require(role['sources'] == source_hashes(), 'Capture sources changed between builds')
    for name, digest in role['files'].items():
        shared.require(shared.sha(dist / name) == digest, 'Published production changed: ' + name)
    old = {tuple(key): value for key, value in before['commands']}
    after = shared.commands(build)
    shared.require(old.keys() == after.keys(), 'Capture fixture changed build graph')
    changed = set()
    for key, command in after.items():
        normalized = [arg for arg in command if arg != '-D' + FLAG + '=1']
        shared.require(old[key] == normalized, 'Capture fixture changed other compiler flags: ' + key[1])
        if old[key] != command:
            changed.add(Path(key[1]).resolve().relative_to(shared.ROOT).as_posix())
    shared.require(changed == {'compat/media/audio/capture.cc'}, 'Unexpected capture fixture compilation boundary')
    # Validate all binaries before creating a usable fixture directory.
    for name in FILES:
        shared.require(SENTINEL in (build / name).read_bytes(), 'Fixture source not compiled: ' + name)
    output.mkdir(parents=True, exist_ok=True)
    payload = dict(shared.context(), role='capture-fixture', fixture=True,
                   compiler=role['compiler'], sources=role['sources'],
                   production=role['files'], files={})
    for source, target in FILES.items():
        shutil.copyfile(build / source, output / target)
        payload['files'][target] = shared.sha(output / target)
    links = shared.seal_selected([(build / source, output / target) for source, target in FILES.items()],
                                role['compiler'], 'capture-fixture')
    if links is not None:
        payload['selected_link'] = links
    (output / 'media-capture-fixture-role.json').write_text(json.dumps(payload, indent=2) + '\n')


def verify(dist, with_fixture):
    context = shared.context()
    normal = json.loads((dist / 'media-capture-production-role.json').read_text())
    shared.require(normal['role'] == 'capture-production' and normal['fixture'] is False, 'Wrong production role')
    shared.require(set(normal['files']) == set(FILES), 'Incomplete production capture inventory')
    shared.require(all(normal[k] == v for k, v in context.items()), 'Production supplier source/run mismatch')
    for name, digest in normal['files'].items():
        shared.require(shared.sha(dist / name) == digest and SENTINEL not in (dist / name).read_bytes(),
                       'Wrong production capture executable: ' + name)
    if with_fixture:
        hook = json.loads((dist / 'media-capture-fixture-role.json').read_text())
        shared.require(hook['role'] == 'capture-fixture' and hook['fixture'] is True, 'Wrong capture fixture role')
        shared.require(all(hook[k] == v for k, v in context.items()), 'Fixture supplier source/run mismatch')
        shared.require(hook['compiler'] == normal['compiler'] and hook['sources'] == normal['sources']
                       and hook['production'] == normal['files'], 'Capture fixture differs from sealed production')
        shared.require(set(hook['files']) == set(FILES.values()), 'Unexpected capture fixture files')
        for name, digest in hook['files'].items():
            shared.require(shared.sha(dist / name) == digest and SENTINEL in (dist / name).read_bytes(),
                           'Wrong synthetic capture executable: ' + name)
        shared.verify_selected(dist, hook.get('selected_link'), hook['compiler'], context,
                               set(FILES.values()), 'capture-fixture')
    print('capture host roles: verified ' + ('production and isolated fixture' if with_fixture else 'production'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('production', 'fixture', 'verify'))
    parser.add_argument('--build', type=Path, default=Path('out'))
    parser.add_argument('--dist', type=Path, default=Path('dist'))
    parser.add_argument('--state', type=Path, default=Path('out/media-capture-state.json'))
    parser.add_argument('--output', type=Path, default=Path('dist-media-capture-test'))
    parser.add_argument('--fixture', action='store_true')
    args = parser.parse_args()
    if args.mode == 'production': production(args.build, args.dist, args.state)
    elif args.mode == 'fixture': fixture(args.build, args.dist, args.state, args.output)
    else: verify(args.dist, args.fixture)


if __name__ == '__main__':
    main()
