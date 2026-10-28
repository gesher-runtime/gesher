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
source = 'tests/guest-isolation/probe.cc'
root = Path('guest-artifact')
identity = {k: os.environ[k] for k in (
    'GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_REPOSITORY', 'GITHUB_REPOSITORY_ID')}
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == identity['GITHUB_SHA']
flags = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
         '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-municode', '-static']
suffix = ['-o', 'guest-artifact/guest-isolation-probe.exe', '-luserenv', '-ladvapi32']
toolchain = 'llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'
if sys.argv[1:] == ['build']:
    assert os.environ['LLVM_MINGW_NAME'] == toolchain
    compiler = Path.home() / 'toolchain' / toolchain / 'bin/x86_64-w64-mingw32-clang++'
    root.mkdir()
    command = [str(compiler), *flags, source, *suffix]
    version = subprocess.check_output([str(compiler), '--version'], text=True)
    compiler_hash = sha(compiler)
    print('ACTUAL_COMPILER', json.dumps(command), compiler_hash, version, flush=True)
    subprocess.run(command, check=True)
    assert sha(compiler) == compiler_hash
    receipt = dict(identity, source_sha256=sha(source), executable_sha256=sha(root / 'guest-isolation-probe.exe'),
                   compiler_sha256=compiler_hash, compiler_version=version, compiler_command=command,
                   toolchain=toolchain)
    (root / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
elif sys.argv[1:] == ['verify']:
    assert sorted(p.name for p in root.iterdir()) == ['guest-isolation-probe.exe', 'receipt.json']
    receipt = json.loads((root / 'receipt.json').read_text(encoding='utf-8'))
    assert set(receipt) == set(identity) | {'source_sha256', 'executable_sha256', 'compiler_sha256',
                                         'compiler_version', 'compiler_command', 'toolchain'}
    assert all(receipt[k] == v for k, v in identity.items())
    assert receipt['source_sha256'] == sha(source)
    assert receipt['executable_sha256'] == sha(root / 'guest-isolation-probe.exe')
    assert receipt['toolchain'] == toolchain
    command = receipt['compiler_command']
    assert command[0].endswith('/toolchain/' + toolchain + '/bin/x86_64-w64-mingw32-clang++')
    assert command[1:] == [*flags, source, *suffix]
    assert len(receipt['compiler_sha256']) == 64 and all(c in '0123456789abcdef' for c in receipt['compiler_sha256'])
    assert 'clang version' in receipt['compiler_version']
else:
    raise RuntimeError('expected build or verify')
print('GUEST_COMPILER_RECEIPT', json.dumps(receipt, sort_keys=True), flush=True)
