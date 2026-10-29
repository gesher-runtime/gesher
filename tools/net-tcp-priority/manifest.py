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
roles = {'gesher-exec.exe': 'host-bionic-loader', 'net_tcp_priority_probe.exe': 'host-posix-contract', 'gesher_arm64.dll': 'matching-arm64-translator', 'tcp-priority-x86_64': 'ndk-x86_64-contract', 'tcp-priority-arm64-v8a': 'ndk-arm64-contract'}
for source, name in [('out/gesher-exec.exe', 'gesher-exec.exe'), ('out/net_tcp_priority_probe.exe', 'net_tcp_priority_probe.exe'), ('out-arm64/gesher_arm64.dll', 'gesher_arm64.dll')]:
    shutil.copyfile(source, Path('tcp-fixture') / name)
compilers = {}
for build, required in [('out', ['compat/posix/src/winsock.cc', 'compat/posix/src/socket.cc', 'compat/posix/src/fd.cc', 'compat/posix/src/system.cc', 'compat/posix/src/epoll.cc', 'compat/posix/src/select.cc', 'compat/bionic/socket_bionic.cc', 'tests/net-tcp-priority/contract.cc']), ('out-arm64', ['compat/arm64/dll/translator.cc'])]:
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
ndk_source = json.loads(Path('tcp-ndk-source.json').read_text())
compilers['ndk'] = json.loads(Path('tcp-guests-compiler.json').read_text())
assert '-DGESHER' in shlex.split(compilers['out']['commands']['tests/net-tcp-priority/contract.cc'])
assert all(not any(arg.startswith('-DGESHER') for arg in compilers['ndk'][abi]['argv']) for abi in ('x86_64', 'arm64-v8a'))
files = subprocess.check_output(['git', 'ls-files', '-z'], text=True).split('\0')
sources = {p: sha(p) for p in files if p and Path(p).is_file()}
readobj = Path(compilers['out']['configured']).parent / 'llvm-readobj'
imports = {}
private_observer = {}
for name in ['gesher-exec.exe', 'net_tcp_priority_probe.exe', 'gesher_arm64.dll']:
    raw = subprocess.check_output([str(readobj), '--coff-imports', str(Path('tcp-fixture') / name)], text=True)
    names = re.findall(r'^\s*Name:\s*(\S+)', raw, re.M)
    imports[name] = names
    print('TCP_PRIORITY_NATIVE_IMPORTS', name, json.dumps(names), flush=True)
    exports = subprocess.check_output([str(readobj), '--coff-exports', str(Path('tcp-fixture') / name)], text=True)
    export_names = re.findall(r'^\s*Name:\s*(\S+)', exports, re.M)
    forbidden = [symbol for symbol in export_names if any(private in symbol for private in ('ConnectWithObserver', 'AwaitConnectRetirement'))]
    assert not forbidden, ('Private connect observer exported', name, forbidden)
    private_observer[name] = dict(exports=len(export_names), observer_exported=False, exports_sha256=hashlib.sha256(exports.encode()).hexdigest())
    print('TCP_PRIORITY_PRIVATE_CONNECT_OBSERVER', name, json.dumps(private_observer[name]), flush=True)
controls()
for name, names in imports.items():
    assert allowed(name, names), ('Wrong native import role', name, names)
outputs = {p.name: dict(role=roles[p.name], sha256=sha(p)) for p in Path('tcp-fixture').iterdir()}
assert set(outputs) == set(roles)
manifest = dict(repository=os.environ['GITHUB_REPOSITORY'], repository_id=os.environ['GITHUB_REPOSITORY_ID'], source=os.environ['GITHUB_SHA'], run=os.environ['GITHUB_RUN_ID'], attempt=os.environ['GITHUB_RUN_ATTEMPT'], producer_job='compile', scope='noninline-only', ndk_archive=ndk_source, import_policy_sha256=sha('tools/net-tcp-priority/import-policy.json'), imports=imports, private_connect_observer=private_observer, compilers=compilers, sources=sources, outputs=outputs)
Path('tcp-fixture/provenance.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('TCP_PRIORITY_PRODUCER', json.dumps({k: manifest[k] for k in ('source', 'run', 'attempt', 'scope')}))
for role, details in compilers.items():
    print('TCP_PRIORITY_COMPILER', role, json.dumps(details))
