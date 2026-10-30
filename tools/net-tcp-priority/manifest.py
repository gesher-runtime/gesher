"""One producer, fresh host+translator+both NDK ELFs, exact output roles."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
from import_policy import allowed, controls

assert os.environ.get('GITHUB_ACTIONS') == 'true'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == os.environ['GITHUB_SHA']
def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()
selection = os.environ['TCP_SUPPLIER_ROLE']
assert selection in ('canonical', 'selected-minimum')
roles = {'gesher-exec.exe': 'host-bionic-loader', 'net_tcp_priority_probe.exe': 'host-posix-contract', 'gesher_arm64.dll': 'matching-arm64-translator', 'tcp-priority-x86_64': 'ndk-x86_64-contract', 'tcp-priority-arm64-v8a': 'ndk-arm64-contract'}
roles.update({'net_udp_guarded_probe.exe':'host-posix-guarded-udp', 'net_udp_guarded_fault_probe.exe':'host-posix-guarded-udp-faults',
    'udp-guarded-x86_64':'ndk-x86_64-guarded-udp', 'udp-guarded-arm64-v8a':'ndk-arm64-guarded-udp',
    'udp-guarded-fault-x86_64':'ndk-x86_64-guarded-udp-faults', 'udp-guarded-fault-arm64-v8a':'ndk-arm64-guarded-udp-faults',
    'udp-guarded-linux.txt':'actual-linux-guarded-udp-reference', 'udp-guarded-fault-linux.txt':'actual-linux-guarded-udp-fault-reference'})
for name in ('net_udp_guarded_probe.exe','net_udp_guarded_fault_probe.exe'):
    shutil.copyfile(Path('out')/name, Path('tcp-fixture')/name)
for source, name in [('out/gesher-exec.exe', 'gesher-exec.exe'), ('out/net_tcp_priority_probe.exe', 'net_tcp_priority_probe.exe'), ('out-arm64/gesher_arm64.dll', 'gesher_arm64.dll')]:
    shutil.copyfile(source, Path('tcp-fixture') / name)
compilers = {}
for build, required in [('out', ['compat/posix/src/winsock.cc', 'compat/posix/src/socket.cc', 'compat/posix/src/fd.cc', 'compat/posix/src/system.cc', 'compat/posix/src/epoll.cc', 'compat/posix/src/select.cc', 'compat/bionic/socket_bionic.cc', 'tests/net-tcp-priority/contract.cc', 'tests/net-tcp-priority/native_udp_control.cc', 'tests/native2/net_udp_trunc.cpp']), ('out-arm64', ['compat/arm64/dll/translator.cc'])]:
    configs = list(Path(build + '/CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    assert len(configs) == 1
    paths = re.findall(r'^\s*set\(CMAKE_CXX_COMPILER\s+"([^"]+)"\)\s*$', configs[0].read_text(), re.M)
    assert len(paths) == 1
    compiler = Path(paths[0]).resolve(strict=True)
    commands = json.loads(Path(build + '/compile_commands.json').read_text())
    selected = {}
    for source in required:
        rows = [r for r in commands if r['file'].replace('\\', '/').endswith('/' + source)]
        assert len(rows) == 1, (build, source, len(rows))
        argv = shlex.split(rows[0]['command'])
        assert paths[0] in argv[:2]
        selected[source] = rows[0]['command']
    compilers[build] = dict(configured=paths[0], resolved=str(compiler), sha256=sha(compiler), version=subprocess.check_output([paths[0], '--version'], text=True), metadata_sha256=sha(configs[0]), commands=selected)
c_configs=list(Path('out/CMakeFiles').glob('*/CMakeCCompiler.cmake'))
assert len(c_configs)==1
c_paths=re.findall(r'^\s*set\(CMAKE_C_COMPILER\s+"([^"]+)"\)\s*$',c_configs[0].read_text(),re.M)
assert len(c_paths)==1
c_commands=json.loads(Path('out/compile_commands.json').read_text())
c_rows=[row for row in c_commands if row['file'].replace('\\','/').endswith('/tests/native2/net_udp_fault_observer.c')]
assert len(c_rows)==1 and c_paths[0] in shlex.split(c_rows[0]['command'])[:2]
compilers['out']['c_guarded']=dict(configured=c_paths[0],resolved=str(Path(c_paths[0]).resolve(strict=True)),
    sha256=sha(c_paths[0]),metadata_sha256=sha(c_configs[0]),source='tests/native2/net_udp_fault_observer.c',
    command=c_rows[0]['command'],version=subprocess.check_output([c_paths[0],'--version'],text=True))
ndk_source = json.loads(Path('tcp-ndk-source.json').read_text())
compilers['ndk'] = json.loads(Path('tcp-guests-compiler.json').read_text())
supplier = json.loads(Path('tcp-supplier-receipt.json').read_text())
expected_supplier = 'canonical-publisher-unmodified-v1' if selection == 'canonical' else 'gesher-winpthreads-selected-minimum-v1'
assert supplier['role'] == expected_supplier and supplier['supplier_selection'] == selection and supplier['producer_job'] == 'compile'
assert supplier['rebuilt_from_reference_source'] == (selection == 'selected-minimum')
assert all(supplier['context'][k] == os.environ[k] for k in supplier['context'])
assert supplier['upstream'] == 'a3d93999ef0521681d45e445c39964a7af0f593f'
assert supplier['patch_sha256'] is None
if selection == 'selected-minimum':
    from selected_supplier import check_selected
    check_selected(supplier, {k:os.environ[k] for k in supplier['context']})
if selection == 'canonical':
    assert supplier['libraries_before'] == supplier['libraries_after'] and supplier['source_before'] == supplier['source_after']
selected_compiler = Path(supplier['compiler_root']).resolve(strict=True)
assert selected_compiler.parent == Path('tcp-canonical-supplier' if selection == 'canonical' else 'tcp-selected-supplier').resolve(strict=True)
assert supplier['public_headers_before'] == supplier['public_headers_after']
assert supplier['compiler_archive']['sha256'] == '5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'
for build in ('out', 'out-arm64'):
    assert Path(compilers[build]['configured']).parent == selected_compiler / 'bin'
    assert compilers[build]['sha256'] == supplier['cxx_driver_sha256']
assert compilers['out']['c_guarded']['configured']==str(selected_compiler/'bin/x86_64-w64-mingw32-clang')
assert compilers['out']['c_guarded']['sha256']==supplier['compiler_driver_sha256']
for relative, digest in supplier['libraries_after'].items():
    assert sha(selected_compiler / 'x86_64-w64-mingw32' / relative) == digest

# Each native target has an independent actual linker trace. Compiler search
# output alone is not evidence that the selected archive entered that link.
native_links = {}
for build, target, name, trace in (
        ('out', 'gesher-exec', 'gesher-exec.exe', 'tcp-link-host-loader.log'),
        ('out', 'net_tcp_priority_probe', 'net_tcp_priority_probe.exe', 'tcp-link-host-probe.log'),
        ('out', 'net_udp_guarded_probe', 'net_udp_guarded_probe.exe', 'tcp-link-guarded.log'),
        ('out', 'net_udp_guarded_fault_probe', 'net_udp_guarded_fault_probe.exe', 'tcp-link-guarded-fault.log'),
        ('out-arm64', 'gesher_arm64', 'gesher_arm64.dll', 'tcp-link-translator.log')):
    commands = subprocess.check_output(['ninja', '-C', build, '-t', 'commands', target], text=True).splitlines()
    links = [row for row in commands if re.search(r'(?:^|\s)-o\s+' + re.escape(name) + r'(?:\s|$)', row)]
    assert len(links) == 1, (name, 'exact configured link command')
    argv = shlex.split(links[0])
    assert compilers[build]['configured'] in argv and '-static' in argv and '-Wl,--verbose' in argv
    text = Path(trace).read_text()
    inspected = {}
    # COFF verbose mode logs the actual opened archive's full path, then
    # separately identifies loaded members by basename and required symbol.
    archives = re.findall(r'^.*?\bReading (\S*(?:libpthread|libwinpthread)\.a)\s*$', text, re.M)
    loaded = re.findall(r'^.*?\bLoaded ((?:libpthread|libwinpthread)\.a\([^\r\n)]+\)) for (\S+)\s*$', text, re.M)
    for archive in archives:
        path = Path(archive)
        if not path.is_absolute():
            path = Path(build) / path
        # Preserve the observed alias basename for Loaded-member correlation,
        # while independently checking its final object stays in this prefix.
        path = Path(os.path.abspath(path))
        resolved = path.resolve(strict=True)
        resolved.relative_to(selected_compiler / 'x86_64-w64-mingw32')
        relative = path.relative_to(selected_compiler / 'x86_64-w64-mingw32').as_posix()
        assert relative in ('lib/libpthread.a', 'lib/libwinpthread.a'), (name, relative)
        assert sha(path) == supplier['libraries_after'][relative]
        inspected[relative] = sha(path)
    loaded_names = {member.split('(', 1)[0] for member, _ in loaded}
    assert loaded_names <= {Path(p).name for p in inspected}
    inputs = {relative: digest for relative, digest in inspected.items() if Path(relative).name in loaded_names}
    assert inputs or name == 'gesher_arm64.dll', (name, 'no static supplier member loaded')
    native_links[name] = dict(command=links[0], trace_sha256=sha(trace),
                              supplier_role='static' if inputs else 'not-linked', inputs=inputs,
                              inspected_archives=inspected,
                              loaded_members=[dict(member=member, symbol=symbol) for member, symbol in loaded])
supplier['native_links'] = native_links
print('TCP_PRIORITY_SUPPLIER_LINKS', json.dumps(native_links), flush=True)
assert '-DGESHER' in shlex.split(compilers['out']['commands']['tests/net-tcp-priority/contract.cc'])
assert all(not any(arg.startswith('-DGESHER') for arg in compilers['ndk'][abi]['argv']) for abi in ('x86_64', 'arm64-v8a'))
files = subprocess.check_output(['git', 'ls-files', '-z'], text=True).split('\0')
sources = {p: sha(p) for p in files if p and Path(p).is_file()}
readobj = Path(compilers['out']['configured']).parent / 'llvm-readobj'
# Linker diagnostics remain CI stdout, not an extra runnable artifact role.
# Bind their exact current binary and configured link command before interpreting
# static library frame names; never symbolize a different executable locally.
map_path = Path('out/tcp-priority-link.map')
map_text = map_path.read_text()
assert map_text.strip()
commands = subprocess.check_output(['ninja', '-C', 'out', '-t', 'commands', 'net_tcp_priority_probe'], text=True).splitlines()
links = [row for row in commands if re.search(r'(?:^|\s)-o\s+net_tcp_priority_probe\.exe(?:\s|$)', row)]
assert len(links) == 1
link_argv = shlex.split(links[0])
assert compilers['out']['configured'] in link_argv and '-Wl,-Map,tcp-priority-link.map' in link_argv
headers = subprocess.check_output([str(readobj), '--file-headers', 'out/net_tcp_priority_probe.exe'], text=True)
bases = re.findall(r'^\s*ImageBase:\s*(0x[0-9a-fA-F]+)\s*$', headers, re.M)
assert len(bases) == 1
selected = [row for row in map_text.splitlines() if any(token in row for token in (
    '__pthread_self_lite', 'pthread_create_wrapper', 'pthread_self', 'pthread_create', 'CreateEvent',
    'pthread_mutex_unlock', 'cleanup', 'spin', 'ConnectOwner', 'ConnectReaper', 'submission_connect'))]
assert all(any(token in row for row in selected) for token in ('pthread_self', 'pthread_create', 'ConnectOwner', 'ConnectReaper'))
assert sha('out/net_tcp_priority_probe.exe') == sha('tcp-fixture/net_tcp_priority_probe.exe')
print('TCP_PRIORITY_LINK_MAP', json.dumps(dict(source=os.environ['GITHUB_SHA'], run=os.environ['GITHUB_RUN_ID'],
    attempt=os.environ['GITHUB_RUN_ATTEMPT'], binary_sha256=sha('out/net_tcp_priority_probe.exe'),
    map_sha256=sha(map_path), image_base=bases[0], link_command=links[0], rows=selected)), flush=True)
imports = {}
private_observer = {}
for name in ['gesher-exec.exe', 'net_tcp_priority_probe.exe', 'gesher_arm64.dll', 'net_udp_guarded_probe.exe', 'net_udp_guarded_fault_probe.exe']:
    raw = subprocess.check_output([str(readobj), '--coff-imports', str(Path('tcp-fixture') / name)], text=True)
    names = re.findall(r'^\s*Name:\s*(\S+)', raw, re.M)
    imports[name] = names
    print('TCP_PRIORITY_NATIVE_IMPORTS', name, json.dumps(names), flush=True)
    exports = subprocess.check_output([str(readobj), '--coff-exports', str(Path('tcp-fixture') / name)], text=True)
    export_names = re.findall(r'^\s*Name:\s*(\S+)', exports, re.M)
    forbidden = [symbol for symbol in export_names if any(private in symbol for private in ('ConnectWithObserver', 'ConnectLegacy', 'PollWithObserver', 'AwaitConnectRetirement', 'ReadConnectOwnerCensus', 'NativeUdpErrorControl', 'DumpOwnHandleTypes', 'NativeSupplierRetirement'))]
    assert not forbidden, ('Private connect observer exported', name, forbidden)
    private_observer[name] = dict(exports=len(export_names), observer_exported=False, exports_sha256=hashlib.sha256(exports.encode()).hexdigest())
    print('TCP_PRIORITY_PRIVATE_CONNECT_OBSERVER', name, json.dumps(private_observer[name]), flush=True)
controls()
for name, names in imports.items():
    assert allowed(name, names), ('Wrong native import role', name, names)
guarded_linux = {}
for name, driver, source, count in (
        ('udp-guarded-linux.txt','g++','tests/native2/net_udp_trunc.cpp',200),
        ('udp-guarded-fault-linux.txt','gcc','tests/native2/net_udp_fault_observer.c',176)):
    rows=(Path('tcp-fixture')/name).read_text().splitlines()
    assert len(rows)==count
    if count==200: assert rows[-1]=='UDP all passed; rows=192; alias=128; stream=1'
    else: assert all(row.startswith('UDPFAULT ') for row in rows)
    driver_path=Path(shutil.which(driver)).resolve(strict=True)
    guarded_linux[name]=dict(driver=str(driver_path), driver_sha256=sha(driver_path),
        version=subprocess.check_output([str(driver_path),'--version'],text=True), source=source,
        source_sha256=sha(source), rows=count, sha256=sha(Path('tcp-fixture')/name))
for abi,machine in (('x86_64',62),('arm64-v8a',183)):
    commands=compilers['ndk'][abi]['guarded_commands']
    assert {c['output'] for c in commands}=={'udp-guarded-'+abi,'udp-guarded-fault-'+abi} and len(commands)==2
    for command in commands:
        raw=(Path('tcp-fixture')/command['output']).read_bytes()
        assert raw[:6]==b'\x7fELF\x02\x01' and int.from_bytes(raw[18:20],'little')==machine
        assert command['sha256']==sha(Path('tcp-fixture')/command['output'])
outputs = {p.name: dict(role=roles[p.name], sha256=sha(p)) for p in Path('tcp-fixture').iterdir()}
assert set(outputs) == set(roles)
manifest = dict(repository=os.environ['GITHUB_REPOSITORY'], repository_id=os.environ['GITHUB_REPOSITORY_ID'], source=os.environ['GITHUB_SHA'], run=os.environ['GITHUB_RUN_ID'], attempt=os.environ['GITHUB_RUN_ATTEMPT'], producer_job='compile', supplier_selection=selection, scope='noninline-only', owned_connect_scope='AF_INET-base-chain1-e70f1aa0-ab8b-11cf-8ca3-00805f48a192-TCP', ndk_archive=ndk_source, supplier=supplier, guarded_linux=guarded_linux, import_policy_sha256=sha('tools/net-tcp-priority/import-policy.json'), imports=imports, private_connect_observer=private_observer, compilers=compilers, sources=sources, outputs=outputs)
Path('tcp-fixture/provenance.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('TCP_PRIORITY_PRODUCER', json.dumps({k: manifest[k] for k in ('source', 'run', 'attempt', 'scope')}))
for role, details in compilers.items():
    print('TCP_PRIORITY_COMPILER', role, json.dumps(details))
