"""Source/AST/memory controls, never a native fixture or real tool operation."""
import ast
import importlib.util
from pathlib import Path
import subprocess
import sys
import hashlib
import io
import json
import runpy

ROOT = Path(__file__).resolve().parents[2]
BASE = '4d54ade1b53483f6d604bd989a7674ea6c376a1f'
def old(path):
    return subprocess.check_output(['git', '-C', str(ROOT), 'show', BASE + ':' + path])
def current(path):
    return (ROOT/path).read_bytes()

for path in ('tests/native/font_environment_test.cc', 'compat/posix/src/font_environment.cc',
             'compat/posix/src/unistd.cc', 'compat/posix/src/path.cc',
             'compat/bionic/native_fonts.cc', 'compat/bionic/shim.cc',
             'compat/posix/posix.cmake', 'compat/posix/include/gesher/font_environment.h',
             'tools/fonts/debugger-preflight.ps1', 'tools/fonts/owner-host-manifest.py',
             '.github/workflows/fonts-debugger-preflight-component.yml',
             '.github/workflows/fonts-debugger-preflight-trusted-source.yml'):
    assert current(path) == old(path), path
addition = current('CMakeLists.txt')[len(old('CMakeLists.txt')):]
assert current('CMakeLists.txt') == old('CMakeLists.txt') + addition
assert addition.count(b'BEGIN FONT_RESOURCE_CHARACTERIZATION_ADDITIVE') == 1
assert addition.count(b'add_executable(') == addition.count(b'EXCLUDE_FROM_ALL') == 1
assert b'target_link_libraries(font_environment_resource_characterization PRIVATE gesher_posix)' in addition
assert addition.endswith(b'# END FONT_RESOURCE_CHARACTERIZATION_ADDITIVE\n')
for name in ('resource-manifest.py', 'resource-import-inventory.py', 'resource-pe-imports.py', 'resource-protocol.py','resource-code-provider.py','resource-code-envelope.py','resource-witness.py'):
    ast.parse(current('tools/fonts/' + name))
spec=importlib.util.spec_from_file_location('resource_manifest', ROOT/'tools/fonts/resource-manifest.py')
module=importlib.util.module_from_spec(spec);sys.path.insert(0,str(ROOT/'tools/fonts'));spec.loader.exec_module(module)
actual='ld.lld: Loaded libgesher_posix.a(unistd.cc.obj) for gesher::HandleFromFd(int)'
assert module.loaded_rows(actual)==[('unistd.cc.obj','gesher::HandleFromFd(int)')]
for malformed in (actual.replace(') for ',') nope '),actual.replace('int)','int) trailing\nld.lld: Loaded libgesher_posix.a(x) bad')):
    try:module.loaded_rows(malformed)
    except AssertionError:pass
    else:raise AssertionError('Malformed actual-format row accepted')
assert module.ROLE=='CURRENT_SOURCE_HOST_FONT_OWNER_RESOURCE_CHARACTERIZATION'
assert module.ROLES['fixture']=='tests/native/font_environment_resource_characterization.cc'
assert len(set(module.INPUTS))==len(module.INPUTS)
assert all((ROOT/name).is_file() for name in module.INPUTS)
native=current(module.ROLES['fixture']).decode()
callback=native.split('void __cdecl Invalid(',1)[1].split('DWORD WINAPI Writer',1)[0]
assert all(token not in callback for token in ('printf','new ','GetModule','std::string','malloc'))
assert 'WaitForSingleObject(state->release,180000)' in callback
assert 'ExitProcess(1)' in native and 'new(std::nothrow) State' in native
assert 'WaitForSingleObject(state->watchdog,10000)' in native
assert 'WaitForSingleObject(state->handler,10000)' in native and 'WaitForSingleObject(state->writer,10000)' in native
runner=current('tools/fonts/resource-characterization.ps1').decode()
assert runner.index('Write-ResourceCommand $stage $captureFile') < runner.index('$child.process.StandardInput.Write("START`n")')
assert "foreach($role in @('child','imports','help','prewarm','capture'))" in runner
assert "foreach($role in @('capture','prewarm','imports','child','help'))" in runner
assert "gallery='NOT_DISABLED'" in runner and "loader='NOT_EXHAUSTIVE'" in runner
assert ' kL 0x14' in runner and 'uf /c' not in runner
fixture = runpy.run_path(str(ROOT/'tools/fonts/test-resource-pe-imports.py'))
decoder = runpy.run_path(str(ROOT/'tools/fonts/resource-import-inventory.py'))
data = bytes(fixture['image']())
entries=[{'name':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()} for name in sorted(fixture['p'].ADMITTED_NAMES)]
header=json.dumps({'entries':entries}).encode()+b'\n'
frames=(data+b'\nFONT_RESOURCE_PE_END\n')*3
assert len(list(decoder['inventory'](io.BytesIO(header+frames),fixture['p'])))==3
negatives=(header+frames[:-1],header+frames+b'x',header+frames.replace(b'PE_END',b'PE_BAD',1),header+frames[:-len(data)],
           json.dumps({'entries':entries[:2]}).encode()+b'\n'+frames,
           json.dumps({'entries':[entries[0]]*3}).encode()+b'\n'+frames,
           header+bytes([data[0]^1])+frames[1:],b' '*16385)
for value in negatives:
    try:list(decoder['inventory'](io.BytesIO(value),fixture['p']))
    except (AssertionError,ValueError):pass
    else:raise AssertionError('Malformed frame accepted')
print('FONT_RESOURCE_SOURCE_PASS old_owner7_old_WCT_old_preflight_exact=1 additive_CMake=1 metadata_only=1 native_processes_started=0')

# Original3-tool passive decoder and native child remain exact; provider is a
# consumer-local signed input, never redistributed by the Linux artifact.
for name in ('resource-import-inventory.py','resource-pe-imports.py'):
    path='tools/fonts/'+name
    assert current(path)==subprocess.check_output(['git','show','a8e79c27639200c3a3ee9162fb053a22727df3f6:'+path])
assert current(module.ROLES['fixture'])==subprocess.check_output(['git','show','a8e79c27639200c3a3ee9162fb053a22727df3f6:'+module.ROLES['fixture']])
assert len(module.INPUTS)==27
assert "'CODE_ENVELOPE_SCRIPT'" in runner and 'resource-code-envelope.py' in runner
assert runner.index('Get-PreflightFileProfile $codePath') < runner.index('$child=Start-ResourceRole')
assert runner.index('Read-ResourceLoadedUCRT $prewarm') < runner.index('$child.process.StandardInput.Write("START`n")')
assert runner.index("$stage.code.cache=$cache[0]") < runner.index('$child=Start-ResourceRole')
assert runner.count("Start-ResourceRole $stage 'imports'")==1
assert not any(word in runner for word in ('uf /c','dt -r','!critsec ','.frame /r','r rcx'))
print('FONT_RESOURCE_CODE_SOURCE_PASS original3decoder_native_exact=1 provider_is_consumer_local=1 source_inputs=27 native_processes_started=0')

# Witness transport does not add a process role or mutate old tools/native code.
assert runner.index("$cold=Wait-ResourceInputReady") < runner.index('$child.process.StandardInput.Write("START`n")')
assert runner.index('$frames=Read-ResourceWitnessFrames') < runner.index('Send-ResourceControlOnce $capture (New-ResourceOwnerTail')
assert "schema=2;role='SIGNED_SYSTEM32_UCRT_CODE'" in runner and '@{version=2;legacy_bytes=' in runner
assert 'fixture_frame_bindings' in current('tools/fonts/resource-manifest.py').decode()
assert not any(token in runner for token in ('dt -r','!locks','!critsec ','.frame /r','r rcx','uf /c'))
print('FONT_RESOURCE_WITNESS_SOURCE_PASS metadata_schema=2 five_roles=UNCHANGED guarded_owner_tail=1 local_native=0')
