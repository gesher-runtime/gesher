"""Source-bound probe supplier with retained target resources; CI only."""
import hashlib,json,os,subprocess,sys,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]; DIST=ROOT/'routing-probe-resource'
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def context():
    assert os.environ['GITHUB_ACTIONS']=='true' and os.environ['GITHUB_REPOSITORY']=='gesher-runtime/gesher'
    assert os.environ['GITHUB_REF']=='refs/heads/wip/pthread-process-routing'
    assert subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()==os.environ['GITHUB_SHA']
    subprocess.check_call(['git','diff','--exit-code','HEAD','--'])
    return {k:os.environ[k] for k in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY')}
def build(llvm):
    ctx=context(); DIST.mkdir(); driver=llvm/'bin/x86_64-w64-mingw32-clang'; receipt=llvm/'gesher-winpthreads-authority.json'
    supplier=json.loads(receipt.read_text()); assert supplier['role']=='gesher-winpthreads-authority-routing-probe-resource-v1'
    assert all(supplier[k]==v for k,v in ctx.items()); lib=llvm/'x86_64-w64-mingw32/lib/libwinpthread.a'; output=DIST/'resource-lease.exe'; source=ROOT/'tests/native/host_pthread_resource_lease_probe.c'
    argv=[str(driver),'-D_WIN32_WINNT=0x0A00','-Wall','-Wextra','-Werror',str(source),str(lib),'-o',str(output)]
    subprocess.check_call(argv); imports=subprocess.check_output([str(llvm/'bin/llvm-readobj'),'--coff-imports',str(output)],text=True)
    assert 'libwinpthread-1.dll' not in imports.lower() and sha(lib)==supplier['files']['lib/libwinpthread.a']
    shutil.copyfile(llvm/'x86_64-w64-mingw32/bin/libwinpthread-1.dll',DIST/'libwinpthread-1.dll'); shutil.copyfile(receipt,DIST/'supplier.json')
    assert sha(DIST/'libwinpthread-1.dll')==supplier['files']['bin/libwinpthread-1.dll']
    data={'context':ctx,'driver':str(driver),'driver_sha256':sha(driver),'source_sha256':sha(source),'argv':argv,'imports':imports,'library_sha256':sha(lib),'members':{p.name:sha(p) for p in DIST.iterdir()}}
    (DIST/'bundle.json').write_text(json.dumps(data,sort_keys=True,indent=2))
def verify():
    ctx=context(); data=json.loads((DIST/'bundle.json').read_text()); supplier=json.loads((DIST/'supplier.json').read_text())
    assert data['context']==ctx and all(supplier[k]==v for k,v in ctx.items())
    assert supplier['role']=='gesher-winpthreads-authority-routing-probe-resource-v1'
    expected={'resource-lease.exe','libwinpthread-1.dll','supplier.json'}
    assert set(data['members'])==expected and {p.name for p in DIST.iterdir()}==expected|{'bundle.json'}
    assert all(sha(DIST/n)==v for n,v in data['members'].items())
    assert data['driver_sha256']==supplier['compiler_driver_sha256'] and data['source_sha256']==sha(ROOT/'tests/native/host_pthread_resource_lease_probe.c')
    assert data['library_sha256']==supplier['files']['lib/libwinpthread.a'] and sha(DIST/'libwinpthread-1.dll')==supplier['files']['bin/libwinpthread-1.dll']
    argv=data['argv']; assert len(argv)==9 and argv[0]==data['driver'] and argv[0].endswith('/bin/x86_64-w64-mingw32-clang')
    assert argv[1:5]==['-D_WIN32_WINNT=0x0A00','-Wall','-Wextra','-Werror']
    assert argv[5].endswith('/tests/native/host_pthread_resource_lease_probe.c') and argv[6].endswith('/x86_64-w64-mingw32/lib/libwinpthread.a')
    assert argv[7]=='-o' and argv[8].endswith('/routing-probe-resource/resource-lease.exe') and 'libwinpthread-1.dll' not in data['imports'].lower()
    print('Verified target-resource role, source, commands and imports')
if __name__=='__main__':
    if sys.argv[1]=='build': build(Path(sys.argv[2]))
    elif sys.argv[1]=='verify': verify()
    else: raise SystemExit(2)
