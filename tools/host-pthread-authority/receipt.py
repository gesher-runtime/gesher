"""Pinned supplier evidence; no compiler or fixture is run outside Actions."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PIN = json.loads((HERE / 'pinned-inputs.json').read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def command(*argv):
    return subprocess.check_output(argv, text=True)


def inputs(source):
    assert command('git', '-C', str(source), 'rev-parse', 'HEAD').strip() == PIN['upstream_commit']
    assert sha(HERE / 'supplier.patch') == PIN['patch_sha256']
    for name, digest in PIN['source_sha256'].items():
        assert sha(source / name) == digest, name


def supplier(source, llvm):
    assert os.environ.get('GITHUB_ACTIONS') == 'true'
    result = {key: os.environ[key] for key in (
        'GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_REPOSITORY')}
    assert result['GITHUB_REPOSITORY'] == 'gesher-runtime/gesher'
    assert command('git', 'rev-parse', 'HEAD').strip() == result['GITHUB_SHA']
    target = llvm / 'x86_64-w64-mingw32'
    files = ['lib/libwinpthread.a', 'lib/libwinpthread.dll.a', 'lib/libpthread.a',
             'lib/libpthread.dll.a', 'bin/libwinpthread-1.dll',
             'share/mingw32/COPYING.winpthreads.txt']
    files += ['include/' + p.name for p in (source / 'mingw-w64-libraries/winpthreads/include').glob('*.h')]
    result.update(role='gesher-winpthreads-authority-v1', pin=PIN,
                  files={name: sha(target / name) for name in files},
                  source_files={str(p.relative_to(source)): sha(p) for p in
                      (source / 'mingw-w64-libraries/winpthreads').rglob('*') if p.is_file()},
                  compiler_archive_sha256=sha(llvm.parent / 'compiler.tar.xz'),
                  compiler=command(str(llvm / 'bin/x86_64-w64-mingw32-clang'), '--version'),
                  cxx_driver_sha256=sha(llvm / 'bin/x86_64-w64-mingw32-clang++'),
                  compiler_driver_sha256=sha(llvm / 'bin/x86_64-w64-mingw32-clang'),
                  compiler_real_sha256=sha((llvm / 'bin/x86_64-w64-mingw32-clang').resolve()),
                  build_logs={name: sha(source / 'build' / name) for name in
                      ('configure-command.log', 'build-commands.log', 'install-commands.log', 'config.log')})
    symbol = 'gesher_winpthread_dup_cpu_handle_v1'
    for name in ('lib/libwinpthread.a', 'lib/libwinpthread.dll.a'):
        output = command(str(llvm / 'bin/llvm-nm'), '--defined-only', str(target / name))
        assert symbol in output, name
        result[name + ':symbols'] = output
    exports = command(str(llvm / 'bin/llvm-readobj'), '--coff-exports', str(target / 'bin/libwinpthread-1.dll'))
    assert symbol in exports
    result['dll_exports'] = exports
    imports = command(str(llvm / 'bin/llvm-readobj'), '--coff-imports', str(target / 'bin/libwinpthread-1.dll'))
    assert 'Name: libwinpthread-1.dll' not in imports, 'Supplier must not import its previous copy'
    result['dll_imports'] = imports
    (llvm / 'gesher-winpthreads-authority.json').write_text(json.dumps(result, indent=2, sort_keys=True))


if __name__ == '__main__':
    if sys.argv[1] == 'inputs':
        inputs(Path(sys.argv[2]))
    elif sys.argv[1] == 'supplier':
        supplier(Path(sys.argv[2]), Path(sys.argv[3]))
    else:
        raise RuntimeError('Unknown receipt operation')
