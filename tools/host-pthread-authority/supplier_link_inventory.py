"""CI-only exact supplier archive cross-TU undefined/defined and PE map."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main(llvm, destination, role):
    assert os.environ['GITHUB_ACTIONS']=='true'
    assert role in ('production','probe')
    llvm=Path(llvm).resolve(); target=llvm/'x86_64-w64-mingw32'
    archive=target/'lib/libwinpthread.a'; dll=target/'bin/libwinpthread-1.dll'
    receipt=llvm/'gesher-winpthreads-authority.json'; supplier=json.loads(receipt.read_text())
    expected={'production':'gesher-winpthreads-authority-v1','probe':'gesher-winpthreads-authority-routing-probe-v1'}
    assert supplier['role']==expected[role]
    for field in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY'): assert supplier[field]==os.environ[field]
    assert sha(archive)==supplier['files']['lib/libwinpthread.a'] and sha(dll)==supplier['files']['bin/libwinpthread-1.dll']
    nm=llvm/'bin/llvm-nm'; readobj=llvm/'bin/llvm-readobj'
    cases={'undefined':[str(nm),'--undefined-only','--print-file-name',str(archive)],
           'defined':[str(nm),'--defined-only','--print-file-name',str(archive)],
           'exports':[str(readobj),'--coff-exports',str(dll)],
           'imports':[str(readobj),'--coff-imports',str(dll)]}
    maps={}
    for name,argv in cases.items():
        output=subprocess.check_output(argv,text=True,errors='strict')
        assert output
        maps[name]={'argv':argv,'text':output,'sha256':hashlib.sha256(output.encode()).hexdigest()}
    result={'role':role,'supplier_receipt_sha256':sha(receipt),'archive_sha256':sha(archive),'dll_sha256':sha(dll),
            'nm_sha256':sha(nm),'readobj_sha256':sha(readobj),'maps':maps,
            'context':{k:os.environ[k] for k in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY')}}
    Path(destination).write_text(json.dumps(result,sort_keys=True,indent=2))
    selected=('_pthread_key_dest','pthread_getclean','_pthread_delay_np_ms','pthread_testcancel','__pthread_shallcancel','pthread_getevent','pthread_self')
    print('SUPPLIER_LINK_INVENTORY role='+role+' archive='+sha(archive)+' dll='+sha(dll),flush=True)
    for line in maps['undefined']['text'].splitlines():
        low=line.casefold()
        if any(x in low for x in ('cond.o','sem.o','rwlock.o','nanosleep.o')) and any(x.casefold() in low for x in selected):
            print('SUPPLIER_CROSS_TU role='+role+' '+line.strip(),flush=True)
if __name__=='__main__': main(*sys.argv[1:])
