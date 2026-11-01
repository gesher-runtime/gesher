"""Source-bound probe-fault retirement bundle; CI only."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
DIST=ROOT/'routing-probe-fault'

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def context():
    assert os.environ['GITHUB_ACTIONS']=='true' and os.environ['GITHUB_REPOSITORY']=='gesher-runtime/gesher'
    assert os.environ['GITHUB_REF']=='refs/heads/wip/pthread-process-routing'
    assert subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()==os.environ['GITHUB_SHA']
    subprocess.check_call(['git','diff','--exit-code','HEAD','--'])
    return {k:os.environ[k] for k in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY')}
def build(llvm):
    ctx=context(); DIST.mkdir()
    driver=llvm/'bin/x86_64-w64-mingw32-clang'; receipt=llvm/'gesher-winpthreads-authority.json'
    supplier=json.loads(receipt.read_text())
    assert supplier['role']=='gesher-winpthreads-authority-routing-probe-fault-v1'
    assert all(supplier[k]==v for k,v in ctx.items())
    commands={}
    for role,archive in [('static','libwinpthread.a'),('dynamic','libwinpthread.dll.a')]:
        lib=llvm/'x86_64-w64-mingw32/lib'/archive; output=DIST/('retirement-'+role+'.exe')
        argv=[str(driver),'-D_WIN32_WINNT=0x0A00','-Wall','-Wextra','-Werror',str(ROOT/'tests/native/host_pthread_retirement_test.c'),str(lib),'-o',str(output)]
        subprocess.check_call(argv)
        imports=subprocess.check_output([str(llvm/'bin/llvm-readobj'),'--coff-imports',str(output)],text=True)
        assert ('libwinpthread-1.dll' in imports.lower())==(role=='dynamic')
        assert sha(lib)==supplier['files']['lib/'+archive]
        commands[role]={'argv':argv,'imports':imports,'library_sha256':sha(lib),'output_sha256':sha(output)}
    shutil.copyfile(llvm/'x86_64-w64-mingw32/bin/libwinpthread-1.dll',DIST/'libwinpthread-1.dll')
    shutil.copyfile(receipt,DIST/'supplier.json')
    assert sha(DIST/'libwinpthread-1.dll')==supplier['files']['bin/libwinpthread-1.dll']
    data={'context':ctx,'driver':str(driver),'driver_sha256':sha(driver),'source_sha256':sha(ROOT/'tests/native/host_pthread_retirement_test.c'),
          'commands':commands,'members':{p.name:sha(p) for p in DIST.iterdir()}}
    (DIST/'bundle.json').write_text(json.dumps(data,sort_keys=True,indent=2))
def verify():
    ctx=context(); data=json.loads((DIST/'bundle.json').read_text()); supplier=json.loads((DIST/'supplier.json').read_text())
    assert data['context']==ctx and all(supplier[k]==v for k,v in ctx.items())
    assert supplier['role']=='gesher-winpthreads-authority-routing-probe-fault-v1'
    expected={'retirement-static.exe','retirement-dynamic.exe','libwinpthread-1.dll','supplier.json'}
    assert set(data['members'])==expected and {p.name for p in DIST.iterdir()}==expected|{'bundle.json'}
    assert all(sha(DIST/n)==h for n,h in data['members'].items())
    assert data['driver_sha256']==supplier['compiler_driver_sha256']
    assert data['source_sha256']==sha(ROOT/'tests/native/host_pthread_retirement_test.c')
    assert sha(DIST/'libwinpthread-1.dll')==supplier['files']['bin/libwinpthread-1.dll']
    assert set(data['commands'])=={'static','dynamic'}
    for role,archive in [('static','libwinpthread.a'),('dynamic','libwinpthread.dll.a')]:
        row=data['commands'][role]; argv=row['argv']
        assert len(argv)==9 and argv[0]==data['driver'] and argv[0].endswith('/bin/x86_64-w64-mingw32-clang')
        assert argv[1:5]==['-D_WIN32_WINNT=0x0A00','-Wall','-Wextra','-Werror']
        assert argv[5].endswith('/tests/native/host_pthread_retirement_test.c')
        assert argv[6].endswith('/x86_64-w64-mingw32/lib/'+archive)
        assert argv[7]=='-o' and argv[8].endswith('/routing-probe-fault/retirement-'+role+'.exe')
        assert row['library_sha256']==supplier['files']['lib/'+archive] and row['output_sha256']==sha(DIST/('retirement-'+role+'.exe'))
        assert ('libwinpthread-1.dll' in row['imports'].lower())==(role=='dynamic')
    print('Verified probe-fault source, roles and imports')
if __name__=='__main__':
    if sys.argv[1]=='build': build(Path(sys.argv[2]))
    elif sys.argv[1]=='verify': verify()
    else: raise SystemExit(2)
