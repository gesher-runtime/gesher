"""CI-only compiler invocation and strict source/run/compiler/executable receipt."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


assert os.environ.get('GITHUB_ACTIONS') == 'true', 'CI only'
source = 'tests/guest-production-launch/probe.cc'
dependencies = [source, 'compat/framework/restricted_launch.h']
root = Path('guest-artifact')
identity = {k: os.environ[k] for k in (
    'GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_REPOSITORY', 'GITHUB_REPOSITORY_ID')}
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == identity['GITHUB_SHA']
flags = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
         '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-municode', '-static']
suffix = ['-o', 'guest-artifact/guest-isolation-probe.exe', '-luserenv', '-ladvapi32']
toolchain = 'llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'
archive_sha = '5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'


def validate(receipt, members):
    assert members == ['guest-isolation-probe.exe', 'receipt.json']
    assert set(receipt) == set(identity) | {'source_sha256', 'executable_sha256', 'compiler_sha256',
                                         'compiler_version', 'compiler_command', 'toolchain', 'archive_sha256'}
    assert all(receipt[k] == v for k, v in identity.items())
    assert receipt['source_sha256'] == {path: sha(path) for path in dependencies}
    assert receipt['executable_sha256'] == sha(root / 'guest-isolation-probe.exe')
    assert receipt['toolchain'] == toolchain and receipt['archive_sha256'] == archive_sha
    command = receipt['compiler_command']
    assert command[0].endswith('/toolchain/' + toolchain + '/bin/x86_64-w64-mingw32-clang++')
    assert command[1:] == [*flags, source, *suffix]
    assert len(receipt['compiler_sha256']) == 64 and all(c in '0123456789abcdef' for c in receipt['compiler_sha256'])
    assert 'clang version' in receipt['compiler_version']


if sys.argv[1:] == ['build']:
    assert os.environ['LLVM_MINGW_NAME'] == toolchain
    compiler = Path.home() / 'toolchain' / toolchain / 'bin/x86_64-w64-mingw32-clang++'
    root.mkdir()
    command = [str(compiler), *flags, source, *suffix]
    version = subprocess.check_output([str(compiler), '--version'], text=True)
    compiler_hash = sha(compiler)
    assert sha('toolchain.tar.xz') == archive_sha
    print('ACTUAL_COMPILER', json.dumps(command), compiler_hash, version, flush=True)
    subprocess.run(command, check=True)
    assert sha(compiler) == compiler_hash
    receipt = dict(identity, source_sha256={path: sha(path) for path in dependencies}, executable_sha256=sha(root / 'guest-isolation-probe.exe'),
                   compiler_sha256=compiler_hash, compiler_version=version, compiler_command=command,
                   toolchain=toolchain, archive_sha256=archive_sha)
    (root / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
elif sys.argv[1:] == ['verify']:
    receipt = json.loads((root / 'receipt.json').read_text(encoding='utf-8'))
    members = sorted(p.name for p in root.iterdir())
    validate(receipt, members)
    # Each gate must actively reject a changed identity, missing/extra field,
    # compiler command or archive member, before the executable is launched.
    cases = [(dict(receipt, **{key: 'invalid'}), members) for key in receipt]
    cases += [(dict(receipt, extra=1), members), ({}, members),
              (receipt, members[:1]), (receipt, members + ['unexpected.exe'])]
    for bad, names in cases:
        try:
            validate(bad, names)
        except (AssertionError, TypeError, KeyError, IndexError):
            continue
        raise RuntimeError('negative receipt control accepted')
    print('PASS strict receipt negative controls', len(cases), flush=True)
else:
    raise RuntimeError('expected build or verify')
print('GUEST_COMPILER_RECEIPT', json.dumps(receipt, sort_keys=True), flush=True)
