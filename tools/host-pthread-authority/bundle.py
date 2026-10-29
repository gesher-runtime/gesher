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
           'host-production-role.json', 'host-fault-role.json'}


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
    assert os.environ.get('GITHUB_REF') == 'refs/heads/wip/host-pthread-authority'
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
    for role, member in (('production', 'supplier.json'), ('fault', 'fault-supplier.json')):
        host = json.loads((DIST / ('host-' + role + '-role.json')).read_text())
        provider = json.loads((DIST / member).read_text())
        assert host['role'] == role and host['test_hook_compiled'] == (role == 'fault')
        assert host['supplier_receipt_sha256'] == sha(DIST / member)
        assert all(host['context'][key] == value for key, value in context().items() if key in host['context'])
        assert all(provider[key] == value for key, value in context().items())
    print('Verified initial host authority bundle; no full/guest/stress qualification implied')


if __name__ == '__main__':
    if sys.argv[1] == 'compile-client':
        compile_client(Path(sys.argv[2]))
    else:
        {'record': record, 'verify': verify}[sys.argv[1]]()
