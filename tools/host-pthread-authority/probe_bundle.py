"""CI-only experimental identity routing bundle; never a production supplier."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
DIST = ROOT / 'routing-probe-fixture'
CASES = {'foreign-dll-callback.exe': ('host_pthread_foreign_dll_callback_test.c', 'libwinpthread.a'),
         'key-lock-gate.exe': ('host_pthread_key_lock_gate_test.c', 'libwinpthread.a'),
         'foreign-native-exit.exe': ('host_pthread_foreign_native_exit_test.c', 'libwinpthread.a'),
         'wrapper-gate.exe': ('host_pthread_wrapper_gate_test.c', 'libwinpthread.a'),
         'retirement-static.exe': ('host_pthread_retirement_test.c', 'libwinpthread.a'),
         'retirement-dynamic.exe': ('host_pthread_retirement_test.c', 'libwinpthread.dll.a'),
         'callback-lifetime.exe': ('host_pthread_callback_lifetime_test.c', 'libwinpthread.a'),
         'key-delete-diagnostic.exe': ('host_pthread_key_delete_diagnostic.c', 'libwinpthread.a'),
         'operations.exe': ('host_pthread_routing_operations.c', 'libwinpthread.a'),
         'strict.exe': ('host_pthread_process_identity_acceptance.c', 'libwinpthread.a'),
         'standalone.exe': ('host_pthread_routing_standalone.c', 'libwinpthread.dll.a'),
         'bad-anchor.exe': ('host_pthread_routing_bad_anchor.c', 'libwinpthread.a'),
         'missing-anchor.exe': ('host_pthread_routing_missing_anchor.c', 'libwinpthread.a')}

def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def context():
    assert os.environ['GITHUB_ACTIONS'] == 'true'
    assert os.environ['GITHUB_REPOSITORY'] == 'gesher-runtime/gesher'
    assert os.environ['GITHUB_REF'] == 'refs/heads/wip/pthread-process-routing'
    assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == os.environ['GITHUB_SHA']
    subprocess.check_call(['git', 'diff', '--exit-code', 'HEAD', '--'])
    return {k: os.environ[k] for k in ('GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_REPOSITORY')}

def build(llvm):
    ctx = context()
    DIST.mkdir()
    subprocess.check_call([sys.executable, str(ROOT / 'tools/host-pthread-authority/runtime_symbol_inventory.py'), str(llvm), str(DIST / 'runtime-symbol-inventory.json')])
    driver = llvm / 'bin/x86_64-w64-mingw32-clang'
    receipt = llvm / 'gesher-winpthreads-authority.json'
    provider = json.loads(receipt.read_text())
    assert provider['role'] == 'gesher-winpthreads-authority-routing-probe-v1'
    assert all(provider[k] == v for k, v in ctx.items())
    module_source=ROOT / 'tests/native/pthread_probe_callback_module.c'
    module_argv=[str(driver), '-D_WIN32_WINNT=0x0A00', '-Wall', '-Wextra', '-Werror', '-shared',
                 str(module_source), '-o', str(DIST / 'probe-callback.dll')]
    subprocess.check_call(module_argv)
    module_exports=subprocess.check_output([str(llvm / 'bin/llvm-readobj'), '--coff-exports', str(DIST / 'probe-callback.dll')], text=True)
    assert 'Name: probe_destructor' in module_exports
    module_imports=subprocess.check_output([str(llvm / 'bin/llvm-readobj'), '--coff-imports', str(DIST / 'probe-callback.dll')], text=True)
    assert 'libwinpthread-1.dll' not in module_imports.lower()
    module={'argv': module_argv, 'source_sha256': sha(module_source), 'output_sha256': sha(DIST / 'probe-callback.dll'), 'exports': module_exports, 'imports': module_imports}
    commands = {}
    for name, (source, archive) in CASES.items():
        lib = llvm / 'x86_64-w64-mingw32/lib' / archive
        argv = [str(driver), '-D_WIN32_WINNT=0x0A00', '-Wall', '-Wextra', '-Werror',
                str(ROOT / 'tests/native' / source), str(lib), '-o', str(DIST / name)]
        subprocess.check_call(argv)
        imports = subprocess.check_output([str(llvm / 'bin/llvm-readobj'), '--coff-imports', str(DIST / name)], text=True)
        assert ('libwinpthread-1.dll' in imports.lower()) == (archive.endswith('.dll.a'))
        assert sha(lib) == provider['files']['lib/' + archive]
        commands[name] = {'argv': argv, 'source_sha256': sha(ROOT / 'tests/native' / source),
                          'library_sha256': sha(lib), 'imports': imports}
    shutil.copyfile(llvm / 'x86_64-w64-mingw32/bin/libwinpthread-1.dll', DIST / 'libwinpthread-1.dll')
    shutil.copyfile(receipt, DIST / 'supplier.json')
    assert sha(DIST / 'libwinpthread-1.dll') == provider['files']['bin/libwinpthread-1.dll']
    members = {p.name: sha(p) for p in DIST.iterdir()}
    (DIST / 'bundle.json').write_text(json.dumps({'context': ctx, 'members': members, 'commands': commands,
                                                  'driver': str(driver), 'driver_sha256': sha(driver), 'callback_module': module}, sort_keys=True, indent=2))

def verify():
    ctx = context()
    data = json.loads((DIST / 'bundle.json').read_text())
    assert data['context'] == ctx
    expected = set(CASES) | {'libwinpthread-1.dll', 'supplier.json', 'probe-callback.dll', 'runtime-symbol-inventory.json'}
    assert set(data['members']) == expected
    assert {p.name for p in DIST.iterdir()} == expected | {'bundle.json'}
    assert all(sha(DIST / name) == digest for name, digest in data['members'].items())
    provider = json.loads((DIST / 'supplier.json').read_text())
    assert provider['role'] == 'gesher-winpthreads-authority-routing-probe-v1'
    assert all(provider[k] == v for k, v in ctx.items())
    assert provider['compiler_driver_sha256'] == data['driver_sha256']
    assert provider['files']['bin/libwinpthread-1.dll'] == sha(DIST / 'libwinpthread-1.dll')
    inventory=json.loads((DIST / 'runtime-symbol-inventory.json').read_text())
    assert inventory['role']=='pinned-toolchain-runtime-symbol-inventory-v1'
    assert all(inventory[k]==ctx[k] for k in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'))
    assert inventory['driver_sha256']==data['driver_sha256'] and inventory['symbols']==['_pthread_key_dest','pthread_getclean']
    assert inventory['rows'] and inventory['roots'][:2]==['x86_64-w64-mingw32/lib','x86_64-w64-mingw32/bin']
    assert len({row['path'] for row in inventory['rows']})==len(inventory['rows'])
    assert inventory['nm_sha256'] and inventory['readobj_sha256']
    for row in inventory['rows']:
        argv=row['command']; path=row['path']
        assert row['sha256'] and '..' not in Path(path).parts and argv[-1].endswith('/'+path)
        assert any(path.startswith(root+'/') for root in inventory['roots'])
        if path.lower().endswith('.dll'):
            assert len(argv)==3 and argv[0].endswith('/bin/llvm-readobj') and argv[1]=='--coff-imports'
        else:
            assert len(argv)==4 and argv[0].endswith('/bin/llvm-nm') and argv[1:3]==['--undefined-only','--print-file-name']
        assert all(any(symbol.casefold() in line.casefold() for symbol in inventory['symbols']) for line in row['hits'])
    module=data['callback_module']; argv=module['argv']
    assert len(argv)==9 and argv[0]==data['driver']
    assert argv[1:6]==['-D_WIN32_WINNT=0x0A00', '-Wall', '-Wextra', '-Werror', '-shared']
    assert argv[6].endswith('/tests/native/pthread_probe_callback_module.c')
    assert argv[7]=='-o' and argv[8].endswith('/routing-probe-fixture/probe-callback.dll')
    assert module['source_sha256']==sha(ROOT / 'tests/native/pthread_probe_callback_module.c')
    assert module['output_sha256']==sha(DIST / 'probe-callback.dll') and 'Name: probe_destructor' in module['exports']
    assert 'libwinpthread-1.dll' not in module['imports'].lower()
    for name, (source, archive) in CASES.items():
        row = data['commands'][name]
        argv = row['argv']
        assert len(argv) == 9 and argv[0] == data['driver']
        assert argv[0].endswith('/bin/x86_64-w64-mingw32-clang')
        assert argv[1:5] == ['-D_WIN32_WINNT=0x0A00', '-Wall', '-Wextra', '-Werror']
        assert argv[5].endswith('/tests/native/' + source)
        assert argv[6].endswith('/x86_64-w64-mingw32/lib/' + archive)
        assert argv[7] == '-o' and argv[8].endswith('/routing-probe-fixture/' + name)
        assert row['source_sha256'] == sha(ROOT / 'tests/native' / source)
        assert row['library_sha256'] == provider['files']['lib/' + archive]
        assert ('libwinpthread-1.dll' in row['imports'].lower()) == archive.endswith('.dll.a')
    print('Verified same-source routing probe bundle; not a production role')

if __name__ == '__main__':
    if sys.argv[1] == 'build': build(Path(sys.argv[2]))
    elif sys.argv[1] == 'verify': verify()
    else: raise SystemExit(2)
