"""Pinned supplier evidence; no compiler or fixture is run outside Actions."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import re
import shlex
import shutil

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
    assert sha(HERE / 'fault-supplier.patch') == PIN['fault_patch_sha256']
    assert sha(HERE / 'deferred-supplier.patch') == PIN['deferred_patch_sha256']
    for name, digest in PIN['source_sha256'].items():
        assert sha(source / name) == digest, name
    makefile = (source / 'mingw-w64-libraries/winpthreads/Makefile.am').read_text()
    flags = re.findall(r'^libwinpthread_la_LDFLAGS (?:\+?=) (.+)$', makefile, re.MULTILINE)
    assert flags == ['-no-undefined -version-info 1:0:0',
                     '-L$(builddir)/fakelib -Wc,-no-pthread'], 'Upstream link flags changed'


def dll_imports(raw):
    names = re.findall(r'^\s*Name:\s*(\S+\.dll)\s*$', raw, re.MULTILINE | re.IGNORECASE)
    lower = [name.casefold() for name in names]
    assert lower and len(set(lower)) == len(lower), 'Missing or duplicate DLL imports'
    assert 'kernel32.dll' in lower, 'Supplier system imports missing'
    unknown = [name for name in lower if name not in {'kernel32.dll', 'ucrtbase.dll'} and
               not re.fullmatch(r'api-ms-win-crt-[a-z]+-l1-1-0\.dll', name)]
    assert not unknown, 'Unapproved supplier imports: ' + str(unknown)
    return names


def selftest():
    assert dll_imports('  Name: KERNEL32.DLL\n  Name: API-MS-WIN-CRT-HEAP-L1-1-0.DLL\n')
    for raw in ('', '  Name: KERNEL32.dll\n  Name: LIBWINPTHREAD-1.DLL\n',
                '  Name: kernel32.dll\n  Name: unexpected.dll\n',
                '  Name: KERNEL32.dll\n  Name: kernel32.DLL\n'):
        try:
            dll_imports(raw)
        except AssertionError:
            continue
        raise AssertionError('Invalid import inventory accepted')
    print('Supplier import negative controls passed: empty, case-self, unknown, duplicate')


def host_role(build, llvm, role, output):
    assert os.environ.get('GITHUB_ACTIONS') == 'true'
    assert role in {'production', 'fault'}
    fault = role == 'fault'
    cache = (build / 'CMakeCache.txt').read_text()
    assert re.findall(r'^GESHER_HOST_CPU_CLOCK_TEST_FAULTS:BOOL=(\S+)$', cache, re.MULTILINE) == [('ON' if fault else 'OFF')]
    compiler_records = list((build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    assert len(compiler_records) == 1, 'Missing or ambiguous generated compiler record'
    generated = compiler_records[0].read_text()
    driver = str(llvm / 'bin/x86_64-w64-mingw32-clang++')
    assert re.findall(r'^set\(CMAKE_CXX_COMPILER "([^"]+)"\)', generated, re.MULTILINE) == [driver]
    assert re.findall(r'^set\(CMAKE_CXX_COMPILER_ID "([^"]+)"\)', generated, re.MULTILINE) == ['Clang']
    rows = json.loads((build / 'compile_commands.json').read_text())
    selected = [r for r in rows if Path(r['file']).resolve() == ROOT / 'compat/posix/src/pthread.cc']
    assert len(selected) == 1
    argv = selected[0].get('arguments') or shlex.split(selected[0]['command'])
    assert Path(selected[0]['directory']).resolve() == build
    launcher = None
    if argv[0] != driver:
        known_launcher = shutil.which('ccache')
        assert known_launcher and Path(argv[0]).is_absolute()
        assert Path(argv[0]).resolve() == Path(known_launcher).resolve() and argv[1] == driver
        launcher = {'path': str(Path(known_launcher).resolve()), 'sha256': sha(Path(known_launcher).resolve())}
    source = ROOT / 'compat/posix/src/pthread.cc'
    assert argv.count('-c') == 1 and Path(argv[argv.index('-c') + 1]).resolve() == source
    assert argv.count(str(source)) == 1
    assert {'-Wall', '-Wextra', '-Werror'} <= set(argv)
    assert not any(arg.startswith('-Wno-error') for arg in argv)
    assert ('-DGESHER_HOST_CPU_CLOCK_TEST_FAULTS=1' in argv) == fault
    assert any('GESHER_HOST_CPU_CLOCK_TEST_FAULTS' in arg for arg in argv) == fault
    obj = build / 'CMakeFiles/gesher_posix.dir/compat/posix/src/pthread.cc.obj'
    symbols = command(str(llvm / 'bin/llvm-nm'), '--defined-only', str(obj))
    assert 'gesher_host_cpu_clock_test_exhaust_ids_v1' not in symbols
    supplier = json.loads((llvm / 'gesher-winpthreads-authority.json').read_text())
    assert supplier['role'] == 'gesher-winpthreads-authority-' + ('fault-v1' if fault else 'v1')
    assert supplier['files']['lib/libwinpthread.a'] == sha(llvm / 'x86_64-w64-mingw32/lib/libwinpthread.a')
    result = {'role': role, 'test_hook_compiled': fault, 'command': argv,
              'configured_compiler': driver, 'compiler_record_sha256': sha(compiler_records[0]),
              'compiler_driver_sha256': sha(Path(driver)), 'launcher': launcher,
              'supplier_receipt_sha256': sha(llvm / 'gesher-winpthreads-authority.json'),
              'object_sha256': sha(obj), 'object_symbols': symbols,
              'gesher_posix_archive_sha256': sha(build / 'libgesher_posix.a'),
              'context': {key: os.environ[key] for key in ('GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_REPOSITORY')}}
    output.write_text(json.dumps(result, indent=2, sort_keys=True))


def supplier(source, llvm, role):
    assert role in {'production', 'fault', 'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}
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
    result.update(role={'production': 'gesher-winpthreads-authority-v1', 'fault': 'gesher-winpthreads-authority-fault-v1', 'probe': 'gesher-winpthreads-authority-routing-probe-v1', 'probe-fault': 'gesher-winpthreads-authority-routing-probe-fault-v1', 'probe-legacy': 'gesher-winpthreads-authority-routing-probe-legacy-v1', 'probe-reaper': 'gesher-winpthreads-authority-routing-probe-reaper-v1', 'probe-resource': 'gesher-winpthreads-authority-routing-probe-resource-v1', 'probe-deferred': 'gesher-winpthreads-authority-routing-probe-deferred-v1'}[role], pin=PIN,
                  files={name: sha(target / name) for name in files},
                  source_files={str(p.relative_to(source)): sha(p) for p in
                      (source / 'mingw-w64-libraries/winpthreads').rglob('*') if p.is_file()},
                  compiler_archive_sha256=sha(llvm.parent / 'compiler.tar.xz'),
                  compiler=command(str(llvm / 'bin/x86_64-w64-mingw32-clang'), '--version'),
                  cxx_driver_sha256=sha(llvm / 'bin/x86_64-w64-mingw32-clang++'),
                  compiler_driver_sha256=sha(llvm / 'bin/x86_64-w64-mingw32-clang'),
                  compiler_real_sha256=sha((llvm / 'bin/x86_64-w64-mingw32-clang').resolve()),
                  underlying_clang_sha256=sha((llvm / 'bin/clang').resolve()),
                  build_logs={name: sha(source / 'build' / name) for name in
                      ('configure-command.log', 'build-commands.log', 'install-commands.log', 'config.log')})
    symbol = 'gesher_winpthread_dup_cpu_handle_v1'
    for name in ('lib/libwinpthread.a', 'lib/libwinpthread.dll.a'):
        output = command(str(llvm / 'bin/llvm-nm'), '--defined-only', str(target / name))
        assert symbol in output, name
        for hook in ('gesher_winpthread_cpu_test_control_v1', 'gesher_winpthread_exit_test_v1', 'gesher_host_cpu_clock_test_exhaust_ids_v1'):
            assert (hook in output) == (role in {'fault', 'probe-fault'}), 'Supplier test symbol role mismatch'
        for deferred in ('gesher_deferred_gate_v1','gesher_deferred_pending_v1'):
            assert (deferred in output) == (role=='probe-deferred'), 'Deferred gate symbol role mismatch'
        for resource in ('gesher_resource_lease_probe_v1','gesher_resource_fault_probe_v1'):
            assert (resource in output) == (role in {'probe-resource','probe-deferred'}), 'Resource symbol role mismatch'
        for reaper in ('gesher_reaper_start_v1','gesher_reaper_stop_v1','gesher_reaper_observe_v1','gesher_reaper_fault_v1'):
            assert (reaper in output) == (role == 'probe-reaper'), 'Reaper symbol role mismatch'
        assert ('gesher_pthread_identity_probe_v6' in output) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'Probe symbol role mismatch'
        assert ('gesher_sync_probe_v1' in output) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'Sync probe symbol role mismatch'
        assert ('gesher_alias_gate_v1' in output) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'Alias gate role mismatch'
        assert ('gesher_admission_probe_v1' in output) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'Admission probe role mismatch'
        for gate in ('gesher_rwlock_init_gate_v1',):
            assert (gate in output) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'Sync init gate role mismatch'
        assert ('gesher_wrapper_probe_gate_v1' in output) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'Wrapper gate role mismatch'
        assert ('gesher_sched_probe_gate_v1' in output) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'Scheduler gate role mismatch'
        for gate in ('gesher_key_probe_arm_v1', 'gesher_key_probe_hold_v1'):
            assert (gate in output) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'Key gate role mismatch'
        assert 'gesher_pthread_identity_probe_v1' not in output and 'gesher_pthread_identity_probe_v2' not in output and 'gesher_pthread_identity_probe_v3' not in output and 'gesher_pthread_identity_probe_v4' not in output and 'gesher_pthread_identity_probe_v5' not in output, 'Stale probe descriptor'
        result[name + ':symbols'] = output
    exports = command(str(llvm / 'bin/llvm-readobj'), '--coff-exports', str(target / 'bin/libwinpthread-1.dll'))
    assert symbol in exports
    for deferred in ('gesher_deferred_gate_v1','gesher_deferred_pending_v1'):
        assert (deferred in exports) == (role=='probe-deferred'), 'Deferred gate export role mismatch'
    for resource in ('gesher_resource_lease_probe_v1','gesher_resource_fault_probe_v1'):
        assert (resource in exports) == (role in {'probe-resource','probe-deferred'}), 'Resource export role mismatch'
    for hook in ('gesher_winpthread_cpu_test_control_v1', 'gesher_winpthread_exit_test_v1', 'gesher_host_cpu_clock_test_exhaust_ids_v1'):
        assert (hook in exports) == (role in {'fault', 'probe-fault'}), 'DLL fault export role mismatch'
    for reaper in ('gesher_reaper_start_v1','gesher_reaper_stop_v1','gesher_reaper_observe_v1','gesher_reaper_fault_v1'):
        assert (reaper in exports) == (role == 'probe-reaper'), 'Reaper export role mismatch'
    assert ('gesher_pthread_identity_probe_v6' in exports) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'DLL probe export role mismatch'
    assert ('gesher_sync_probe_v1' in exports) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'DLL sync probe export role mismatch'
    assert ('gesher_alias_gate_v1' in exports) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'DLL alias gate role mismatch'
    assert ('gesher_admission_probe_v1' in exports) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'DLL admission probe role mismatch'
    for gate in ('gesher_rwlock_init_gate_v1',):
        assert (gate in exports) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'DLL sync init gate role mismatch'
    assert ('gesher_wrapper_probe_gate_v1' in exports) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'DLL wrapper gate role mismatch'
    assert ('gesher_sched_probe_gate_v1' in exports) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'DLL scheduler gate role mismatch'
    for gate in ('gesher_key_probe_arm_v1', 'gesher_key_probe_hold_v1'):
        assert (gate in exports) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'DLL key gate role mismatch'
    assert 'gesher_pthread_identity_probe_v1' not in exports and 'gesher_pthread_identity_probe_v2' not in exports and 'gesher_pthread_identity_probe_v3' not in exports and 'gesher_pthread_identity_probe_v4' not in exports and 'gesher_pthread_identity_probe_v5' not in exports, 'Stale probe descriptor'
    result['dll_exports'] = exports
    imports = command(str(llvm / 'bin/llvm-readobj'), '--coff-imports', str(target / 'bin/libwinpthread-1.dll'))
    result['dll_import_names'] = dll_imports(imports)
    result['dll_imports'] = imports
    build_log = (source / 'build/build-commands.log').read_text()
    assert ('-DGESHER_PTHREAD_ROUTING_PROBE=1' in build_log) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'Probe compile role mismatch'
    for flag in ('-DGESHER_CANCEL_STATE_PROBE=1','-DGESHER_DEFERRED_ONLY_PROBE=1'):
        assert (flag in build_log) == (role=='probe-deferred'), 'Deferred capability compile flag mismatch'
    assert ('-DGESHER_RESOURCE_LEASE_PROBE=1' in build_log) == (role in {'probe-resource','probe-deferred'}), 'Resource flag mismatch'
    assert ('-DGESHER_REAPER_PROBE=1' in build_log) == (role == 'probe-reaper'), 'Reaper flag mismatch'
    assert ('-DGESHER_RWLOCK_ORIGINAL_RECHECK=1' in build_log) == (role == 'probe-legacy'), 'Original recheck role mismatch'
    rwlock_compile=[line for line in build_log.splitlines() if 'src/rwlock.c' in line and (' -c ' in line or ' -c	' in line)]
    assert rwlock_compile and any('-DDLL_EXPORT' in line for line in rwlock_compile), 'Missing rwlock DLL compile'
    assert all(('-DGESHER_RWLOCK_ORIGINAL_RECHECK=1' in line)==(role=='probe-legacy') for line in rwlock_compile), 'Rwlock compile role mismatch'
    dll_thread = [line for line in build_log.splitlines() if 'src/thread.c' in line and '-DDLL_EXPORT' in line]
    assert dll_thread, 'Missing actual DLL thread compile'
    assert all(('-DGESHER_PTHREAD_ROUTING_PROBE=1' in line) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}) for line in dll_thread), 'DLL probe flag mismatch'
    layouts = re.findall(r'^\s*0 \| struct _pthread_v\n(.*?\[sizeof=\d+, align=\d+\])',
                         build_log, re.MULTILINE | re.DOTALL)
    assert layouts, 'No actual private ABI layout from this supplier compilation'
    layout = layouts[-1]
    if role=='probe-deferred': print('SUPPLIER_ACTUAL_LAYOUT deferred-only\n'+layout,flush=True)
    for field in ('valid', 'ret_arg', 'func', 'clean', 'nobreak', 'h', 'cpu_handle',
                  'cpu_generation', 'evStart', 'p_clock', 'cancelled', 'in_cancel',
                  'thread_noposix', 'p_state', 'keymax', 'keyval', 'keyval_set',
                  'thread_name', 'spin_keys', 'tid', 'rwlc', 'rwlq', 'sched_pol',
                  'ended', 'sched', 'jb', 'next', 'x'):
        assert re.search(r'\b' + field + r'\s*$', layout, re.MULTILINE), 'Missing ABI field: ' + field
    assert re.search(r'^\s*48 \|\s+HANDLE cpu_handle$', layout, re.MULTILINE)
    assert re.search(r'^\s*56 \|\s+unsigned long long cpu_generation$', layout, re.MULTILINE)
    assert ('[sizeof=544, align=16]' if role=='probe-deferred' else '[sizeof=512, align=16]' if role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'} else '[sizeof=496, align=16]') in layout
    for field,offset in (('operation_refs',120),('clock_destroy_pending',124)):
        assert bool(re.search(r'\b'+field+r'$',layout,re.MULTILINE)) == (role in {'probe-resource','probe-deferred'}), 'Resource field role mismatch'
        assert bool(re.search(r'^\s*'+str(offset)+r' \|\s+[^\n]*\b'+field+r'$',layout,re.MULTILINE)) == (role in {'probe-resource','probe-deferred'}), 'Resource field layout mismatch'
    if role=='probe-deferred':
        offsets={'valid':0,'ret_arg':8,'func':16,'clean':24,'nobreak':32,'h':40,'cpu_handle':48,'cpu_generation':56,'evStart':64,'p_clock':72,'p_state':84,'keymax':88,'keyval':96,'keyval_set':104,'keyval_generation':112,'operation_refs':120,'clock_destroy_pending':124,'cancel_state_lock':128,'lifecycle':136,'join_claimed_by':144,'cleanup_refs':152,'pending_retire':156,'thread_name':160,'spin_keys':168,'tid':176,'rwlc':180,'rwlq':184,'sched_pol':248,'ended':252,'sched':256,'jb':272,'next':528,'x':536}
        for field,offset in offsets.items():
            assert re.search(r'^\s*'+str(offset)+r' \|\s+[^\n]*\b'+field+r'$',layout,re.MULTILINE), 'Deferred full field offset mismatch: '+field
        for field,bits in (('cancelled','80:0-1'),('in_cancel','80:2-3'),('thread_noposix','80:4-5')):
            assert re.search(r'^\s*'+re.escape(bits)+r' \|\s+int '+field+r'$',layout,re.MULTILINE), 'Deferred bitfield layout mismatch: '+field
    for field,offset in (('cancel_state_lock',128),('lifecycle',136),('join_claimed_by',144)):
        assert bool(re.search(r'\b'+field+r'$',layout,re.MULTILINE)) == (role=='probe-deferred'), 'Deferred field role mismatch'
        if role=='probe-deferred': assert re.search(r'^\s*'+str(offset)+r' \|\s+[^\n]*\b'+field+r'$',layout,re.MULTILINE), 'Deferred field offset mismatch'
    if role=='probe-deferred':
        for path,digest in PIN['deferred_source_sha256'].items():
            actual=sha(source/path); print('DEFERRED_SOURCE_HASH '+path+' '+actual,flush=True)
            assert actual==digest, 'Deferred actual compiler source mismatch'
    assert ('keyval_generation' in layout) == (role in {'probe', 'probe-fault', 'probe-legacy', 'probe-reaper', 'probe-resource', 'probe-deferred'}), 'Probe generation layout mismatch'
    result['private_record_layout'] = '0 | struct _pthread_v\n' + layout
    (llvm / 'gesher-winpthreads-authority.json').write_text(json.dumps(result, indent=2, sort_keys=True))


if __name__ == '__main__':
    if sys.argv[1] == 'selftest':
        selftest()
    elif sys.argv[1] == 'inputs':
        inputs(Path(sys.argv[2]))
    elif sys.argv[1] == 'supplier':
        supplier(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4])
    elif sys.argv[1] == 'host-role':
        host_role(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve(), sys.argv[4], Path(sys.argv[5]))
    else:
        raise RuntimeError('Unknown receipt operation')
