"""Current-run identity for actual UDP ABI binaries and the Linux result supplier."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import re

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "net-dns"))
from payload_shared import authenticated_archive, source_inventory

ROLES = {
    'x86_64/net_udp_trunc': 'bionic-x86_64-actual-udp',
    'arm64-v8a/net_udp_trunc': 'bionic-arm64-actual-udp',
    'x86_64/net_udp_fault': 'bionic-x86_64-actual-udp-faults',
    'arm64-v8a/net_udp_fault': 'bionic-arm64-actual-udp-faults',
    'net-udp/linux-reference.txt': 'linux-libc-executed-reference',
    'net-udp/linux-fault-reference.txt': 'linux-libc-executed-fault-reference',
    'net-udp/ndk-compiler.txt': 'ndk-compiler-identity',
    'net-udp/linux-compiler.txt': 'linux-compiler-identity',
    'net-udp/compilers.json': 'actual-authenticated-ndk-compiler-commands',
}

def pairs(rows):
    value = {}
    for key, item in rows:
        if key in value:
            raise RuntimeError('Duplicate JSON member')
        value[key] = item
    return value


def read(path):
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=pairs)


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def identity():
    if os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('CI only')
    fields = {'source': 'GITHUB_SHA', 'run': 'GITHUB_RUN_ID', 'attempt': 'GITHUB_RUN_ATTEMPT',
              'repository': 'GITHUB_REPOSITORY', 'repository_id': 'GITHUB_REPOSITORY_ID'}
    result = {k: os.environ[v] for k, v in fields.items()}
    if subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() != result['source']:
        raise RuntimeError('Checkout source mismatch')
    result['fixture_sha256'] = digest(Path('tests/native2/net_udp_trunc.cpp'))
    result['fault_fixture_sha256'] = digest(Path('tests/native2/net_udp_fault_observer.c'))
    result['sources'] = source_inventory()
    return result

mode, directory = sys.argv[1:]
root = Path(directory)
manifest = root / 'net-udp/manifest.json'
if mode == 'compile':
    records = {'context': identity(), 'guests': {}}
    with authenticated_archive(Path('dns-ndk.zip')) as (ndk, archive):
        records['archive'] = archive
        binpath = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
        for abi, triple in (('x86_64', 'x86_64-linux-android'), ('arm64-v8a', 'aarch64-linux-android')):
            records['guests'][abi] = []
            for source, output, language in (('tests/native2/net_udp_trunc.cpp', 'net_udp_trunc', '++'), ('tests/native2/net_udp_fault_observer.c', 'net_udp_fault', '')):
                driver = binpath / (triple + '29-clang' + language)
                command = [str(driver), '-std=c++20' if language else '-std=c11', '-O2', '-fstack-protector-strong', '-Wall', '-Wextra', '-Werror']
                if language: command.append('-static-libstdc++')
                command += [source, '-o', str(root / abi / output)]
                subprocess.run(command, check=True, timeout=120)
                records['guests'][abi].append({'command': command, 'driver_sha256': digest(driver), 'clang_sha256': digest((binpath / ('clang' + language)).resolve()), 'version': subprocess.check_output([str(driver), '--version'], text=True), 'output': abi + '/' + output, 'sha256': digest(root / abi / output)})
    (root / 'net-udp').mkdir(exist_ok=True)
    (root / 'net-udp/compilers.json').write_text(json.dumps(records, sort_keys=True) + '\n')
    sys.exit(0)
rows = {name: {'role': role, 'sha256': digest(root / name)} for name, role in ROLES.items()}
expected = {'schema': 1, **identity(), 'producer': 'native2 / native2', 'members': rows}
if mode == 'create':
    manifest.write_text(json.dumps(expected, indent=2) + '\n', encoding='utf-8')
elif mode == 'verify':
    actual = list((root / 'net-udp').iterdir())
    if any(not p.is_file() or p.is_symlink() for p in actual) or {p.name for p in actual} != {Path(n).name for n in ROLES if n.startswith('net-udp/')} | {'manifest.json'}:
        raise RuntimeError('Unexpected UDP whole asset members')
    if read(manifest) != expected:
        raise RuntimeError('UDP source/run/attempt/roles/members/digest mismatch')
else:
    raise RuntimeError('Unknown mode')
for path, machine in [('x86_64/net_udp_trunc', 62), ('arm64-v8a/net_udp_trunc', 183),
                      ('x86_64/net_udp_fault', 62), ('arm64-v8a/net_udp_fault', 183)]:
    elf = (root / path).read_bytes()
    if elf[:6] != b'\x7fELF\x02\x01' or int.from_bytes(elf[18:20], 'little') != machine:
        raise RuntimeError('Wrong actual ELF ABI')
reference = (root / 'net-udp/linux-reference.txt').read_text(encoding='utf-8').splitlines()
if len(reference) != 200 or sum(' api=' in row for row in reference) != 192 or reference[-1] != 'UDP all passed; rows=192; alias=128; stream=1':
    raise RuntimeError('Missing actual Linux reference rows')
if 'clang version' not in (root / 'net-udp/ndk-compiler.txt').read_text(encoding='utf-8'):
    raise RuntimeError('Missing compiler identity')
fault_rows = (root / 'net-udp/linux-fault-reference.txt').read_text(encoding='utf-8').splitlines()
if len(fault_rows) != 176 or not all(row.startswith('UDPFAULT ') for row in fault_rows):
    raise RuntimeError('Missing actual Linux fault/metadata rows')

records = read(root / 'net-udp/compilers.json')
if records['context'] != identity() or records['archive']['publisher_sha1'] != '090e8083a715fdb1a3e402d0763c388abb03fb4e' or records['archive']['size'] != 663987688 or not re.fullmatch('[0-9a-f]{64}', records['archive']['sha256']):
    raise RuntimeError('UDP authenticated compiler identity differs')
if set(records['guests']) != {'x86_64', 'arm64-v8a'}:
    raise RuntimeError('UDP actual compiler ABI roles differ')
for abi, commands in records['guests'].items():
    if len(commands) != 2 or {c['output'] for c in commands} != {abi + '/net_udp_trunc', abi + '/net_udp_fault'}:
        raise RuntimeError('UDP actual compiler output roles differ')
    for c in commands:
        if c['sha256'] != digest(root / c['output']) or not {'-Wall', '-Wextra', '-Werror'} <= set(c['command']) or 'clang version' not in c['version'] or not re.fullmatch('[0-9a-f]{64}', c['clang_sha256']):
            raise RuntimeError('UDP actual compiler command differs')

print('PASS UDP supplier exact source/run/attempt/compiler/roles/digests; actual ELF ABIs; Linux rows=192; authenticated fresh NDK')
