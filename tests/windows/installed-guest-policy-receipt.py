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
source = 'tests/windows/installed-guest-policy-fixture.cc'
dependencies = [source, 'compat/framework/installed_guest.h', 'compat/framework/restricted_launch.h', 'compat/binder/launch_control.h', 'compat/posix/src/startup_null.h', 'compat/posix/src/fd.cc', 'compat/posix/src/system_properties.cc', 'compat/posix/include/gesher/system_properties.h']
root = Path('guest-policy-artifact')
identity = {k: os.environ[k] for k in (
    'GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_REPOSITORY', 'GITHUB_REPOSITORY_ID')}
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == identity['GITHUB_SHA']
flags = ['-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-fexceptions',
         '-DGESHER_PROPERTY_DIAGNOSTIC', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-municode', '-static']
suffix = ['-o', 'guest-policy-artifact/installed-guest-policy-fixture.exe', '-ladvapi32']
toolchain = 'llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'
archive_sha = '5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'


def validate(receipt, members):
    assert members == ['installed-guest-policy-fixture.exe', 'receipt.json']
    assert set(receipt) == set(identity) | {'source_sha256', 'executable_sha256', 'compiler_sha256',
                                         'compiler_version', 'compiler_command', 'toolchain', 'archive_sha256'}
    assert all(receipt[k] == v for k, v in identity.items())
    assert receipt['source_sha256'] == {path: sha(path) for path in dependencies}
    assert receipt['executable_sha256'] == sha(root / 'installed-guest-policy-fixture.exe')
    assert receipt['toolchain'] == toolchain and receipt['archive_sha256'] == archive_sha
    command = receipt['compiler_command']
    assert command[0].endswith('/toolchain/' + toolchain + '/bin/x86_64-w64-mingw32-clang++')
    assert command[1:] == [*flags, source, 'compat/posix/src/system_properties.cc', *suffix]
    assert receipt['compiler_sha256'] == 'c9b86311ade81d53235c93fafabdf98328d094de344ea9a9038e0dab6695ee9f'
    assert 'clang version' in receipt['compiler_version']


if sys.argv[1:] == ['build']:
    assert os.environ['LLVM_MINGW_NAME'] == toolchain
    compiler = Path.home() / 'toolchain' / toolchain / 'bin/x86_64-w64-mingw32-clang++'
    root.mkdir()
    command = [str(compiler), *flags, source, 'compat/posix/src/system_properties.cc', *suffix]
    version = subprocess.check_output([str(compiler), '--version'], text=True)
    compiler_hash = sha(compiler)
    assert sha('toolchain.tar.xz') == archive_sha
    print('ACTUAL_COMPILER', json.dumps(command), compiler_hash, version, flush=True)
    subprocess.run(command, check=True)
    assert sha(compiler) == compiler_hash
    receipt = dict(identity, source_sha256={path: sha(path) for path in dependencies}, executable_sha256=sha(root / 'installed-guest-policy-fixture.exe'),
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
