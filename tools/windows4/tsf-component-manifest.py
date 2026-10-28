"""CI-only provenance for the actual configured TSF targets."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

assert os.environ.get('GITHUB_ACTIONS') == 'true'
def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == os.environ['GITHUB_SHA']
for path, blob in {'compat/input/text_store.cc':'f1068f05954c0173837fbc70a86eda681edab61d', 'probes/windows4/text_store_probe.cc':'ea76cb9ccd00c50b143747e204c8aa2f4959e5a5'}.items():
    assert subprocess.check_output(['git', 'hash-object', path], text=True).strip() == blob
records = list(Path('out/CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
assert len(records) == 1
matches = re.findall(r'^\s*set\(CMAKE_CXX_COMPILER\s+"([^"]+)"\)\s*$', records[0].read_text(), re.M)
assert len(matches) == 1
compiler = Path(matches[0]).resolve(strict=True)
version = subprocess.check_output([matches[0], '--version'], text=True).strip()
assert version
rows = json.loads(Path('out/compile_commands.json').read_text())
commands = {}
for target, sources in {'windows4_text_store_probe':['probes/windows4/text_store_probe.cc','compat/input/text_store.cc','compat/input/text_service.cc'], 'windows4_tsf_service':['probes/windows4/tsf_service_dll.cc']}.items():
    for source in sources:
        selected = [r for r in rows if r['file'].replace('\\','/').endswith('/' + source) and ('CMakeFiles/' + target + '.dir/') in r['command'].replace('\\','/')]
        assert len(selected) == 1, (target, source, len(selected))
        argv = shlex.split(selected[0]['command'])
        assert matches[0] in argv[:2], argv[:2]
        commands[target + ':' + source] = selected[0]['command']
allowed = {'kernel32.dll','user32.dll','ole32.dll','uuid.dll','ntdll.dll','advapi32.dll','shell32.dll','gdi32.dll','msvcrt.dll','ucrtbase.dll','combase.dll','api-ms-win-core-synch-l1-2-0.dll'} | {'api-ms-win-crt-' + part + '-l1-1-0.dll' for part in ('stdio','runtime','locale','heap','private','string','convert','math','environment','filesystem','time','process','utility')}
for name in ['windows4_text_store_probe.exe','windows4_tsf_service.dll']:
    names = re.findall(r'^\s*Name:\s*(\S+)', Path('fixture/' + name + '.imports.txt').read_text(), re.M)
    assert names and not (set(x.lower() for x in names) - allowed), (name, names)
    print('TSF_IMPORTS', name, ','.join(names))
files = subprocess.check_output(['git','ls-files','-z']).decode().split('\0')
sources = {p: sha(p) for p in files if p and Path(p).is_file()}
outputs = {p.name: sha(p) for p in Path('fixture').iterdir() if p.is_file()}
assert len(outputs) == 4
manifest = dict(repository=os.environ['GITHUB_REPOSITORY'], repository_id=os.environ['GITHUB_REPOSITORY_ID'], run=os.environ['GITHUB_RUN_ID'], attempt=os.environ['GITHUB_RUN_ATTEMPT'], source=os.environ['GITHUB_SHA'], compiler=dict(configured=matches[0], resolved=str(compiler), sha256=sha(compiler), metadata_sha256=sha(records[0]), version=version, commands=commands), sources=sources, outputs=outputs)
Path('fixture/provenance.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('TSF_CONFIGURED_COMPILER', json.dumps(manifest['compiler']))
