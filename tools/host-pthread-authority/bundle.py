"""Source/run-bound narrow host supplier bundle, distinct from guest qualification."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
DIST = ROOT / 'host-authority-fixture'
MEMBERS = {'host_pthread_authority_test.exe', 'host_pthread_supplier_test.exe',
           'libwinpthread-1.dll', 'supplier.json', 'COPYING.winpthreads.txt',
           'imports-host.txt', 'imports-dynamic.txt', 'compiler.json',
           'host_pthread_authority_fault_test.exe', 'fault-supplier.json',
           'host-production-role.json', 'host-fault-role.json',
           'retirement-static.exe', 'retirement-dynamic.exe', 'fault-libwinpthread-1.dll',
           'retirement-compiler.json'}


def compile_retirement(llvm):
    context()
    driver = llvm / 'bin/x86_64-w64-mingw32-clang'
    src = ROOT / 'tests/native/host_pthread_retirement_test.c'
    rows = {}
    for role, archive in (('static', 'libwinpthread.a'), ('dynamic', 'libwinpthread.dll.a')):
        library = llvm / 'x86_64-w64-mingw32/lib' / archive
        output = DIST / ('retirement-' + role + '.exe')
        argv = [str(driver), '-D_WIN32_WINNT=0x0A00', '-Wall', '-Wextra', '-Werror',
                str(src), str(library), '-o', str(output)]
        subprocess.run(argv, check=True)
        imports = subprocess.check_output([str(llvm / 'bin/llvm-readobj'), '--coff-imports', str(output)], text=True)
        assert ('libwinpthread-1.dll' in imports.lower()) == (role == 'dynamic')
        rows[role] = dict(command=argv, library_sha256=sha(library), output_sha256=sha(output), imports=imports)
    data = dict(context=context(), source_sha256=sha(src), compiler_driver_sha256=sha(driver),
                configured_driver=str(driver), supplier_receipt_sha256=sha(llvm / 'gesher-winpthreads-authority.json'), roles=rows)
    (DIST / 'retirement-compiler.json').write_text(json.dumps(data, sort_keys=True, indent=2))


def compile_client(llvm):
    context()
    driver = llvm / 'bin/x86_64-w64-mingw32-clang'
    cxx = llvm / 'bin/x86_64-w64-mingw32-clang++'
    command = [str(driver), '-D_WIN32_WINNT=0x0A00', '-Wall', '-Wextra', '-Werror',
               str(ROOT / 'tests/native/host_pthread_supplier_test.c'),
               str(llvm / 'x86_64-w64-mingw32/lib/libwinpthread.dll.a'), '-lbcrypt',
               '-o', str(DIST / 'host_pthread_supplier_test.exe')]
    subprocess.run(command, check=True)
    (DIST / 'compiler.json').write_text(json.dumps({
        'configured_driver': str(cxx), 'sha256': sha(cxx), 'client_command': command,
        'client_driver_sha256': sha(driver)}, sort_keys=True))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def context():
    assert os.environ.get('GITHUB_ACTIONS') == 'true'
    c = {k: os.environ[k] for k in ('GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_REPOSITORY')}
    assert c['GITHUB_REPOSITORY'] == 'gesher-runtime/gesher'
    assert os.environ.get('GITHUB_REF') == 'refs/heads/wip/pthread-external-exit'
    assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == c['GITHUB_SHA']
    subprocess.check_call(['git', 'diff', '--exit-code', 'HEAD', '--'])
    return c


def source():
    names = subprocess.check_output(['git', 'ls-files', '-z']).decode().split('\0')
    return {p: sha(ROOT / p) for p in names if p}


def record():
    assert set(p.name for p in DIST.iterdir()) == MEMBERS
    data = context()
    data.update(role='host-authority-initial-controls-v1', sources=source(),
                members={p: sha(DIST / p) for p in MEMBERS})
    supplier = json.loads((DIST / 'supplier.json').read_text())
    assert all(supplier[k] == data[k] for k in context())
    compiler = json.loads((DIST / 'compiler.json').read_text())
    commands = json.loads((ROOT / 'out/compile_commands.json').read_text())
    expected = compiler['configured_driver']
    for path in ('compat/posix/src/pthread.cc', 'tests/native/host_pthread_authority_test.cc'):
        rows = [r for r in commands if Path(r['file']).resolve() == (ROOT / path).resolve()]
        assert len(rows) == 1
        import shlex
        argv = shlex.split(rows[0]['command'])
        assert argv[0] == expected or (argv[0].endswith('ccache') and argv[1] == expected)
    assert sha(Path(expected)) == compiler['sha256'] == supplier['cxx_driver_sha256']
    assert compiler['client_driver_sha256'] == supplier['compiler_driver_sha256']
    assert 'libwinpthread-1.dll' not in (DIST / 'imports-host.txt').read_text().lower()
    assert 'libwinpthread-1.dll' in (DIST / 'imports-dynamic.txt').read_text().lower()
    (DIST / 'bundle.json').write_text(json.dumps(data, sort_keys=True, indent=2))


def verify():
    data = json.loads((DIST / 'bundle.json').read_text())
    assert all(data[k] == v for k, v in context().items())
    assert data['role'] == 'host-authority-initial-controls-v1'
    assert data['sources'] == source()
    assert set(p.name for p in DIST.iterdir()) == MEMBERS | {'bundle.json'}
    assert data['members'] == {p: sha(DIST / p) for p in MEMBERS}
    supplier = json.loads((DIST / 'supplier.json').read_text())
    assert supplier['files']['bin/libwinpthread-1.dll'] == sha(DIST / 'libwinpthread-1.dll')
    assert supplier['files']['share/mingw32/COPYING.winpthreads.txt'] == sha(DIST / 'COPYING.winpthreads.txt')
    retirement = json.loads((DIST / 'retirement-compiler.json').read_text())
    fault_supplier = json.loads((DIST / 'fault-supplier.json').read_text())
    assert retirement['context'] == context()
    assert retirement['source_sha256'] == sha(ROOT / 'tests/native/host_pthread_retirement_test.c')
    assert retirement['compiler_driver_sha256'] == fault_supplier['compiler_driver_sha256']
    assert retirement['supplier_receipt_sha256'] == sha(DIST / 'fault-supplier.json')
    assert set(retirement['roles']) == {'static', 'dynamic'}
    for role, archive in (('static', 'libwinpthread.a'), ('dynamic', 'libwinpthread.dll.a')):
        row = retirement['roles'][role]
        argv = row['command']
        assert len(argv) == 9 and argv[0] == retirement['configured_driver']
        assert argv[1:5] == ['-D_WIN32_WINNT=0x0A00', '-Wall', '-Wextra', '-Werror']
        assert argv[5].endswith('/tests/native/host_pthread_retirement_test.c')
        assert argv[6].endswith('/x86_64-w64-mingw32/lib/' + archive)
        assert argv[7] == '-o' and argv[8].endswith('/host-authority-fixture/retirement-' + role + '.exe')
        assert row['library_sha256'] == fault_supplier['files']['lib/' + archive]
        assert row['output_sha256'] == sha(DIST / ('retirement-' + role + '.exe'))
        assert ('libwinpthread-1.dll' in row['imports'].lower()) == (role == 'dynamic')
    for role, member in (('production', 'supplier.json'), ('fault', 'fault-supplier.json')):
        host = json.loads((DIST / ('host-' + role + '-role.json')).read_text())
        provider = json.loads((DIST / member).read_text())
        assert host['role'] == role and host['test_hook_compiled'] == (role == 'fault')
        if role == 'fault':
            assert provider['files']['bin/libwinpthread-1.dll'] == sha(DIST / 'fault-libwinpthread-1.dll')
        assert host['supplier_receipt_sha256'] == sha(DIST / member)
        assert host['context'] == context()
        assert provider['role'] == 'gesher-winpthreads-authority-' + ('fault-v1' if role == 'fault' else 'v1')
        assert all(provider[key] == value for key, value in context().items())
    print('Verified initial host authority bundle; no full/guest/stress qualification implied')


if __name__ == '__main__':
    if sys.argv[1] == 'compile-client':
        compile_client(Path(sys.argv[2]))
    elif sys.argv[1] == 'compile-retirement':
        compile_retirement(Path(sys.argv[2]))
    else:
        {'record': record, 'verify': verify}[sys.argv[1]]()
