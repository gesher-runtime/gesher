"""Distinct pristine-source minimum candidate; compiler work is Actions-only."""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PIN = json.loads((HERE / 'minimum-inputs.json').read_text())
ROLE = 'gesher-winpthreads-selected-minimum-v1'
CORE = ('pthread_self', 'pthread_create', 'pthread_join', '_pthread_tryjoin',
        'pthread_exit', 'pthread_key_create', 'pthread_key_delete',
        'pthread_getspecific', 'pthread_setspecific', '__pth_gpointer_locked', 'DllMain')
OFFSETS = dict(valid=0, ret_arg=8, func=16, clean=24, nobreak=32, h=40,
               evStart=48, p_clock=56, p_state=68, keymax=72, keyval=80,
               keyval_set=88, thread_name=96, spin_keys=104, tid=112,
               rwlc=116, rwlq=120, sched_pol=184, ended=188, sched=192,
               jb=208, next=464, x=472)
CHUNK_BYTES = 2304


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def command(*argv):
    return subprocess.check_output(argv, text=True)


def receipt_lines(payload):
    """Bounded ASCII transport for exact archived receipt bytes, not a summary."""
    count = (len(payload) + CHUNK_BYTES - 1) // CHUNK_BYTES
    manifest = dict(label='minimum-supplier', bytes=len(payload), parts=count,
                    chunk_bytes=CHUNK_BYTES, sha256=hashlib.sha256(payload).hexdigest())
    lines = ['MINIMUM_RECEIPT_BEGIN ' + json.dumps(manifest, sort_keys=True)]
    for index in range(count):
        part = payload[index * CHUNK_BYTES:(index + 1) * CHUNK_BYTES]
        lines.append('MINIMUM_RECEIPT_PART ' + json.dumps(dict(
            label=manifest['label'], index=index, parts=count, bytes=len(part),
            sha256=hashlib.sha256(part).hexdigest(), data=base64.b64encode(part).decode('ascii')), sort_keys=True))
    lines.append('MINIMUM_RECEIPT_END ' + json.dumps(manifest, sort_keys=True))
    assert all(line.isascii() and len(line) <= 4096 for line in lines)
    return lines


def reconstruct_receipt(lines):
    assert lines and all(line.isascii() and len(line) <= 4096 for line in lines), 'Long/non-ASCII receipt transport line'
    assert lines[0].startswith('MINIMUM_RECEIPT_BEGIN ') and lines[-1].startswith('MINIMUM_RECEIPT_END ')
    manifest = json.loads(lines[0].split(' ', 1)[1])
    assert json.loads(lines[-1].split(' ', 1)[1]) == manifest
    assert set(manifest) == {'label', 'bytes', 'parts', 'chunk_bytes', 'sha256'}
    assert manifest['label'] == 'minimum-supplier' and manifest['chunk_bytes'] == CHUNK_BYTES
    assert manifest['parts'] == (manifest['bytes'] + CHUNK_BYTES - 1) // CHUNK_BYTES
    assert len(lines) == manifest['parts'] + 2
    payload = bytearray()
    for index, line in enumerate(lines[1:-1]):
        assert line.startswith('MINIMUM_RECEIPT_PART ')
        part = json.loads(line.split(' ', 1)[1])
        assert set(part) == {'label', 'index', 'parts', 'bytes', 'sha256', 'data'}
        assert part['label'] == manifest['label'] and part['index'] == index and part['parts'] == manifest['parts']
        raw = base64.b64decode(part['data'], validate=True)
        expected = min(CHUNK_BYTES, manifest['bytes'] - len(payload))
        assert len(raw) == part['bytes'] == expected
        assert hashlib.sha256(raw).hexdigest() == part['sha256']
        payload.extend(raw)
    assert len(payload) == manifest['bytes'] and hashlib.sha256(payload).hexdigest() == manifest['sha256']
    return bytes(payload)


def context():
    assert os.environ.get('GITHUB_ACTIONS') == 'true'
    assert os.environ['GITHUB_REPOSITORY'] == 'gesher-runtime/gesher'
    assert os.environ['GITHUB_REPOSITORY_ID'] == '1426153186'
    if os.environ.get('GITHUB_JOB')=='compile':
        assert os.environ['GITHUB_REF'] == 'refs/heads/wip/tcp-minimum-selection'
        assert os.environ['GITHUB_WORKFLOW'] == 'net-tcp-selected-component'
        assert os.environ['TCP_SUPPLIER_ROLE'] == 'selected-minimum'
    elif os.environ.get('GESHER_HOST_PTHREAD_CALLER')=='coherent-selected-full/build':
        sys.path.insert(0,str(ROOT/'tools/toolchain'))
        import coherent_selected_context as coherent
        coherent.context(os.environ,'selected-minimum')
    elif os.environ.get('GESHER_HOST_PTHREAD_CALLER')=='pthread-first-link/build':
        sys.path.insert(0,str(ROOT/'tools/toolchain'))
        import first_link_context
        first_link_context.context(os.environ,'selected-minimum')
    elif os.environ.get('GESHER_HOST_PTHREAD_CALLER')=='pthread-native-off-receiver/build':
        sys.path.insert(0,str(ROOT/'tools/toolchain'))
        import native_off_receiver_context as native
        native.context(os.environ,'selected-minimum')
    else:
        assert os.environ['GITHUB_REF'] == 'refs/heads/wip/pthread-production-factory'
        assert os.environ['GITHUB_WORKFLOW'] == 'ci' and os.environ['GITHUB_JOB']=='build'
        assert os.environ['GESHER_HOST_PTHREAD_CALLER']=='ci/build'
    if os.environ['GITHUB_JOB'] != 'compile':
        assert os.environ['GESHER_HOST_PTHREAD_SELECTION'] in ('publisher-control','selected-minimum')
    assert command('git', 'rev-parse', 'HEAD').strip() == os.environ['GITHUB_SHA']
    subprocess.check_call(['git', 'diff', '--exit-code', 'HEAD', '--'])
    return {k: os.environ[k] for k in ('GITHUB_SHA', 'GITHUB_RUN_ID',
                                      'GITHUB_RUN_ATTEMPT', 'GITHUB_REPOSITORY')}


def inputs(source, patched=False):
    context()
    assert command('git', '-C', str(source), 'rev-parse', 'HEAD').strip() == PIN['upstream_commit']
    for name, digest in PIN['ordered_patch_sha256'].items():
        assert sha(HERE / name) == digest
    expected = PIN['output_source_sha256'] if patched else PIN['source_sha256']
    for name, digest in expected.items():
        actual = sha(source / name)
        print('MINIMUM_SOURCE ' + name + ' expected=' + digest + ' actual=' + actual, flush=True)
        assert actual == digest, name
    changed = command('git', '-C', str(source), 'diff', '--name-only').splitlines()
    assert changed == (['mingw-w64-libraries/winpthreads/src/thread.c'] if patched else [])


def exports(text):
    result = re.findall(r'^\s*Name: (\S+)\s*$', text, re.MULTILINE)
    assert result and len(result) == len(set(result)), 'Missing/duplicate actual exports'
    return sorted(result)


def imports(text):
    names = re.findall(r'^\s*Name: (\S+\.dll)\s*$', text, re.MULTILINE | re.IGNORECASE)
    names = [x.casefold() for x in names]
    assert names and len(names) == len(set(names)) and 'kernel32.dll' in names
    assert all(n in ('kernel32.dll', 'ucrtbase.dll') or
               re.fullmatch(r'api-ms-win-crt-[a-z]+-l1-1-0\.dll', n) for n in names)
    return sorted(names)


def validate_layout(layout):
    assert '[sizeof=480, align=16]' in layout, 'Minimum must use original ABI480, not private496/512/560'
    for name, offset in OFFSETS.items():
        assert re.search(r'^\s*' + str(offset) + r' \|\s+[^\n]*\b' + name + r'$', layout, re.MULTILINE), name
    for name, bits in (('cancelled', '64:0-1'), ('in_cancel', '64:2-3'),
                       ('thread_noposix', '64:4-5')):
        assert re.search(r'^\s*' + re.escape(bits) + r' \|\s+int ' + name + r'$', layout, re.MULTILINE), name
    for name in ('cpu_handle', 'cpu_generation', 'keyval_generation', 'cleanup_refs',
                 'operation_refs', 'pending_retire', 'cancel_state_lock', 'lifecycle',
                 'tls_owner_refs', 'kernel_finalized', 'retire_next'):
        assert not re.search(r'\b' + name + r'$', layout, re.MULTILINE), name


def actual_macros(text):
    rows = dict(re.findall(r'^#define ([A-Za-z_][A-Za-z_0-9]*) (.+)$', text, re.MULTILINE))
    result = {}
    for name in ('_WIN32_WINNT', 'WINVER'):
        value = rows[name]
        seen = set()
        while value in rows:
            assert value not in seen
            seen.add(value)
            value = rows[value]
        value = value.strip('()')
        assert re.fullmatch(r'0[xX][0-9a-fA-F]+|[0-9]+', value), (name, value)
        result[name] = int(value, 0)
    assert result['_WIN32_WINNT'] == 0x0601, 'Unexpected original configured SDK target'
    assert result['WINVER'] >= 0x0600, 'GetThreadId profile requires Vista-or-later scope'
    assert not any(n.startswith('GESHER_') for n in rows), 'Private capability leaked into minimum profile'
    return result


def map_symbols(text, names):
    found = {}
    owner = None
    for line in text.splitlines():
        row = re.fullmatch(r'([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+[0-9]+ (.*)', line)
        if not row:
            continue
        tail = row.group(2)
        if ':(' in tail:
            owner = tail.strip().rsplit(':(', 1)[0]
        elif tail.startswith('                '):
            name = tail.strip()
            if name in names:
                definition = (owner, row.group(1).lower())
                assert name not in found or found[name] == definition, 'Conflicting DLL symbol owner/RVA: ' + name
                found[name] = definition
        else:
            owner = None
    assert set(found) == set(names), 'Unresolved actual minimum DLL symbols: ' + str(set(names) - set(found))
    return found


def original(llvm, source, output):
    ctx = context()
    assert os.environ['GITHUB_JOB'] in ('compile','build')
    target = llvm / 'x86_64-w64-mingw32'
    assert not (llvm / 'gesher-winpthreads-authority.json').exists()
    assert sha(llvm.parent / 'compiler.tar.xz') == PIN['compiler_archive_sha256']
    assert sha(target / 'lib/libwinpthread.a') == PIN['publisher_static_sha256']
    assert sha(target / 'lib/libpthread.a') == PIN['publisher_static_sha256']
    actual_sdk=sha(target / 'include/processthreadsapi.h')
    print('MINIMUM_ACTUAL_SDK_HEADER processthreadsapi.h expected='+PIN['sdk_processthreadsapi_sha256']+' actual='+actual_sdk,flush=True)
    assert actual_sdk == PIN['sdk_processthreadsapi_sha256']
    headers = {p.name: sha(p) for p in (source / 'mingw-w64-libraries/winpthreads/include').glob('*.h')}
    for name, digest in headers.items():
        actual = sha(target / 'include' / name)
        print('MINIMUM_ORIGINAL_HEADER ' + name + ' expected=' + digest + ' actual=' + actual, flush=True)
        assert actual == digest, name
    files = {n: sha(target / n) for n in ('lib/libwinpthread.a', 'lib/libpthread.a',
             'lib/libwinpthread.dll.a', 'lib/libpthread.dll.a', 'bin/libwinpthread-1.dll')}
    raw_exports = command(str(llvm / 'bin/llvm-readobj'), '--coff-exports', str(target / 'bin/libwinpthread-1.dll'))
    raw_imports = command(str(llvm / 'bin/llvm-readobj'), '--coff-imports', str(target / 'bin/libwinpthread-1.dll'))
    record = dict(context=ctx, role='publisher-reference-unmodified-v1',
                  compiler_archive_sha256=PIN['compiler_archive_sha256'], files=files,
                  public_headers=headers, dll_exports=exports(raw_exports),
                  dll_import_names=imports(raw_imports), dll_exports_raw=raw_exports,
                  dll_imports_raw=raw_imports,
                  cxx_driver_sha256=sha(llvm / 'bin/x86_64-w64-mingw32-clang++'),
                  compiler_driver_sha256=sha(llvm / 'bin/x86_64-w64-mingw32-clang'))
    output.write_text(json.dumps(record, sort_keys=True, indent=2))


def supplier(source, llvm, original_file):
    ctx = context()
    assert os.environ['GITHUB_JOB']=='compile' or (os.environ['GITHUB_JOB']=='build' and os.environ['GESHER_HOST_PTHREAD_SELECTION']=='selected-minimum')
    inputs(source, patched=True)
    baseline = json.loads(original_file.read_text())
    assert baseline['context'] == ctx and baseline['role'] == 'publisher-reference-unmodified-v1'
    build = source / 'build'
    log = (build / 'build-commands.log').read_text()
    install_log = (build / 'install-commands.log').read_text()
    assert not re.search(r'-D(?:GESHER_|WINPTHREAD_ROUTING)', log)
    assert '-g -O2 -mguard=cf -Werror -Xclang -fdump-record-layouts' in log
    assert str(build) == (build / 'actual-build-directory.txt').read_text().strip()
    layouts = re.findall(r'^\s*0 \| struct _pthread_v\n(.*?\[sizeof=\d+, align=\d+\])', log, re.MULTILINE | re.DOTALL)
    assert layouts
    layout = layouts[-1]
    print('MINIMUM_ACTUAL_LAYOUT\n' + layout, flush=True)
    validate_layout(layout)
    profiles = {}
    for prefix in ('sdk', 'supplier'):
        macro_file = build / (prefix + '-profile-macros.txt')
        machine = (build / (prefix + '-profile-machine.txt')).read_text()
        assert 'IMAGE_FILE_MACHINE_AMD64' in machine
        profiles[prefix] = actual_macros(macro_file.read_text())
    print('MINIMUM_ACTUAL_SDK_PROFILE ' + json.dumps(profiles, sort_keys=True), flush=True)
    assert profiles['sdk'] == profiles['supplier']
    target = llvm / 'x86_64-w64-mingw32'
    names = ('lib/libwinpthread.a', 'lib/libpthread.a', 'lib/libwinpthread.dll.a',
             'lib/libpthread.dll.a', 'bin/libwinpthread-1.dll')
    files = {n: sha(target / n) for n in names}
    assert files['lib/libpthread.a'] == files['lib/libwinpthread.a']
    assert files['lib/libpthread.dll.a'] == files['lib/libwinpthread.dll.a']
    assert files['lib/libwinpthread.a'] != baseline['files']['lib/libwinpthread.a']
    symbols = {}
    for name in ('lib/libwinpthread.a', 'lib/libwinpthread.dll.a'):
        raw = command(str(llvm / 'bin/llvm-nm'), '--defined-only', str(target / name))
        assert 'gesher_' not in raw.lower(), 'Private probe/supplier capability leaked'
        assert 'pthread_join' in raw and '_pthread_tryjoin' in raw
        symbols[name] = raw
    raw_exports = command(str(llvm / 'bin/llvm-readobj'), '--coff-exports', str(target / 'bin/libwinpthread-1.dll'))
    raw_imports = command(str(llvm / 'bin/llvm-readobj'), '--coff-imports', str(target / 'bin/libwinpthread-1.dll'))
    assert exports(raw_exports) == baseline['dll_exports'], 'Public DLL export surface changed'
    assert imports(raw_imports) == baseline['dll_import_names'], 'Original DLL dependency surface changed'
    assert re.search(r'\bGetThreadId\b', raw_imports), 'Native self API missing from actual DLL'
    for name, digest in baseline['public_headers'].items():
        assert sha(target / 'include' / name) == digest
    map_text = (build / 'supplier-dll.map').read_text()
    print('MINIMUM_DLL_RAW_MAP_BEGIN\n' + map_text + 'MINIMUM_DLL_RAW_MAP_END', flush=True)
    definitions = map_symbols(map_text, CORE)
    reading = set(re.findall(r'\bReading ([^\r\n]+\.(?:o|obj))\r?$', log + '\n' + install_log, re.MULTILINE))
    objects = {}
    for name, (owner, rva) in definitions.items():
        assert owner
        matches = {str((Path(p) if Path(p).is_absolute() else build / p).resolve())
                   for p in reading if Path(p).name == Path(owner).name}
        assert len(matches) == 1, 'Unresolved/ambiguous actual DLL input origin: ' + name
        path = Path(next(iter(matches)))
        assert path.is_file() and path.is_relative_to(build.resolve()) and path.name.endswith('thread.o')
        objects[name] = dict(input=owner, rva=rva, actual_reading_path=str(path), sha256=sha(path))
    assert len({v['actual_reading_path'] for v in objects.values()}) == 1
    dll_compile = [line for line in log.splitlines() if 'src/thread.c' in line and '-DDLL_EXPORT' in line]
    assert dll_compile and all('GESHER_' not in line for line in dll_compile)
    compiled_paths=set()
    for line in dll_compile:
        tokens=shlex.split(line)
        if '-c' in tokens and '-o' in tokens:
            name=Path(tokens[tokens.index('-o')+1])
            compiled_paths.add(str((name if name.is_absolute() else build/name).resolve()))
    assert {row['actual_reading_path'] for row in objects.values()}<=compiled_paths,'DLL map object was not the actual DLL_EXPORT source compile output'
    record = dict(ctx, role=ROLE, pin=PIN, files=files, source_files={n: sha(source / n) for n in PIN['output_source_sha256']},
                  compiler_archive_sha256=sha(llvm.parent / 'compiler.tar.xz'),
                  compiler_driver_sha256=sha(llvm / 'bin/x86_64-w64-mingw32-clang'),
                  cxx_driver_sha256=sha(llvm / 'bin/x86_64-w64-mingw32-clang++'),
                  compiler=command(str(llvm / 'bin/x86_64-w64-mingw32-clang'), '--version'),
                  original=baseline, public_headers=baseline['public_headers'],
                  private_record_layout='0 | struct _pthread_v\n' + layout,
                  original_profile=profiles, library_symbols=symbols, dll_exports=raw_exports,
                  dll_imports=raw_imports, dll_symbol_objects=objects, dll_compile_commands=dll_compile,
                  build_logs={n: sha(build / n) for n in ('configure-command.log', 'build-commands.log',
                      'install-commands.log', 'config.log', 'supplier-dll.map', 'sdk-profile-macros.txt',
                      'supplier-profile-macros.txt', 'sdk-profile-machine.txt', 'supplier-profile-machine.txt',
                      'sdk-profile.c', 'sdk-profile.obj', 'supplier-profile.obj')})
    assert record['compiler_archive_sha256'] == PIN['compiler_archive_sha256']
    assert record['compiler_driver_sha256'] == baseline['compiler_driver_sha256']
    assert record['cxx_driver_sha256'] == baseline['cxx_driver_sha256']
    payload = json.dumps(record, sort_keys=True, indent=2).encode('utf-8')
    (llvm / 'gesher-winpthreads-authority.json').write_bytes(payload)
    for line in receipt_lines(payload):
        print(line, flush=True)


def profile(build):
    context()
    assert os.environ['GITHUB_JOB']=='compile' or (os.environ['GITHUB_JOB']=='build' and os.environ['GESHER_HOST_PTHREAD_SELECTION']=='selected-minimum')
    profiles={}
    for prefix in ('sdk','supplier'):
        assert 'IMAGE_FILE_MACHINE_AMD64' in (build/(prefix+'-profile-machine.txt')).read_text()
        profiles[prefix]=actual_macros((build/(prefix+'-profile-macros.txt')).read_text())
        assert (build/(prefix+'-profile.obj')).stat().st_size>0
    assert profiles['sdk']==profiles['supplier']
    print('MINIMUM_PREBUILD_TYPED_SDK_PROFILE '+json.dumps(profiles,sort_keys=True),flush=True)


def selftest():
    layout='\n'.join(str(offset)+' | int '+name for name,offset in OFFSETS.items())
    layout+='\n64:0-1 | int cancelled\n64:2-3 | int in_cancel\n64:4-5 | int thread_noposix\n[sizeof=480, align=16]'
    validate_layout(layout)
    rejected=0
    bad_layouts=[layout.replace(str(offset)+' | int '+name,str(offset+8)+' | int '+name) for name,offset in OFFSETS.items()]
    bad_layouts += [layout.replace('[sizeof=480','[sizeof=496'),layout+'\n40 | HANDLE cpu_handle',layout.replace('64:0-1','64:1-2')]
    for bad in bad_layouts:
        try:validate_layout(bad)
        except AssertionError:rejected+=1
        else:raise AssertionError('Original ABI mismatch accepted')
    macros='#define _WIN32_WINNT 0x0601\n#define WINVER _WIN32_WINNT\n'
    assert actual_macros(macros)==dict(_WIN32_WINNT=1537,WINVER=1537)
    for bad in (macros.replace('0x0601','0x0A00'),macros.replace('0x0601','unknown'),macros+'#define GESHER_CANCEL_STATE_PROBE 1\n'):
        try:actual_macros(bad)
        except (AssertionError,KeyError):rejected+=1
        else:raise AssertionError('Private/unresolved SDK profile accepted')
    assert imports('  Name: KERNEL32.dll\n  Name: ucrtbase.dll\n')
    for bad in ('','  Name: kernel32.dll\n  Name: libwinpthread-1.dll\n','  Name: kernel32.dll\n  Name: KERNEL32.dll\n'):
        try:imports(bad)
        except AssertionError:rejected+=1
        else:raise AssertionError('Bad minimum DLL import inventory accepted')
    text='00001000 00000020     4         src/.libs/libwinpthread_la-thread.o:(.text)\n'
    rows={name:'00001000 00000000     0                 '+name+'\n' for name in CORE}
    text+=''.join(rows.values());assert set(map_symbols(text,CORE))==set(CORE)
    assert map_symbols(text+rows['pthread_join'],CORE)==map_symbols(text,CORE)
    for bad in (text.replace(rows['pthread_self'],''),text+rows['pthread_join'].replace('00001000','00002000',1),text+'00001000 00000020     4         other.o:(.text)\n'+rows['pthread_self']):
        try:map_symbols(bad,CORE)
        except AssertionError:rejected+=1
        else:raise AssertionError('Unresolved/conflicting minimum DLL map accepted')
    payload=json.dumps({'long':'x'*40000,'unicode':'\u05d0\u05d1','newline':'a\nb'},ensure_ascii=False).encode('utf-8')
    lines=receipt_lines(payload);assert reconstruct_receipt(lines)==payload
    assert reconstruct_receipt(receipt_lines(b''))==b''
    controls=[lines[:1]+lines[2:],lines[:2]+lines[1:],lines[:-1],lines[:1]+list(reversed(lines[1:-1]))+lines[-1:],lines[:1]+['x'*4097]+lines[2:],lines[:1]+['not-ascii-\u05d0']+lines[2:]]
    for field,value in [('index',1),('parts',999),('bytes',1),('sha256','0'*64),('data','AAAA')]:
        bad=list(lines);part=json.loads(bad[1].split(' ',1)[1]);part[field]=value;bad[1]='MINIMUM_RECEIPT_PART '+json.dumps(part);controls.append(bad)
    for bad in controls:
        try:reconstruct_receipt(bad)
        except (AssertionError,ValueError,KeyError,TypeError):rejected+=1
        else:raise AssertionError('Incomplete/tampered/long receipt transport accepted')
    print('PASS minimum source-only480ABI/SDK/no-private/import/DLL-map negatives='+str(rejected)+'; no compiler/native/network operation')


if __name__ == '__main__':
    operation = sys.argv[1]
    if operation == 'selftest':
        selftest()
    elif operation == 'inputs':
        inputs(Path(sys.argv[2]))
    elif operation == 'patched':
        inputs(Path(sys.argv[2]), patched=True)
    elif operation == 'original':
        original(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
    elif operation == 'supplier':
        supplier(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
    elif operation == 'profile':
        profile(Path(sys.argv[2]))
    else:
        raise RuntimeError('Unknown minimum receipt operation')
