"""Current-run identity for actual UDP ABI binaries and the Linux result supplier."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROLES = {
    'x86_64/net_udp_trunc': 'bionic-x86_64-actual-udp',
    'arm64-v8a/net_udp_trunc': 'bionic-arm64-actual-udp',
    'net-udp/linux-reference.txt': 'linux-libc-executed-reference',
    'net-udp/ndk-compiler.txt': 'ndk-compiler-identity',
    'net-udp/linux-compiler.txt': 'linux-compiler-identity',
}

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
    return result

mode, directory = sys.argv[1:]
root = Path(directory)
manifest = root / 'net-udp/manifest.json'
rows = {name: {'role': role, 'sha256': digest(root / name)} for name, role in ROLES.items()}
expected = {'schema': 1, **identity(), 'producer': 'native2 / native2', 'members': rows}
if mode == 'create':
    manifest.write_text(json.dumps(expected, indent=2) + '\n', encoding='utf-8')
elif mode == 'verify':
    if json.loads(manifest.read_text(encoding='utf-8')) != expected:
        raise RuntimeError('UDP source/run/attempt/roles/members/digest mismatch')
else:
    raise RuntimeError('Unknown mode')
for path, machine in [('x86_64/net_udp_trunc', 62), ('arm64-v8a/net_udp_trunc', 183)]:
    elf = (root / path).read_bytes()
    if elf[:6] != b'\x7fELF\x02\x01' or int.from_bytes(elf[18:20], 'little') != machine:
        raise RuntimeError('Wrong actual ELF ABI')
reference = (root / 'net-udp/linux-reference.txt').read_text(encoding='utf-8').splitlines()
if len(reference) != 200 or sum(' api=' in row for row in reference) != 192 or reference[-1] != 'UDP all passed; rows=192; alias=128; stream=1':
    raise RuntimeError('Missing actual Linux reference rows')
if 'clang version' not in (root / 'net-udp/ndk-compiler.txt').read_text(encoding='utf-8'):
    raise RuntimeError('Missing compiler identity')
print('PASS UDP supplier exact source/run/attempt/compiler/roles/digests; actual ELF ABIs; Linux rows=192')
