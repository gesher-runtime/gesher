"""CI-only inventory of pinned toolchain runtime consumers for two symbols."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

SYMBOLS=('_pthread_key_dest','pthread_getclean')
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def run(llvm, output):
    assert os.environ['GITHUB_ACTIONS']=='true'
    llvm=llvm.resolve(); target=llvm/'x86_64-w64-mingw32'; driver=llvm/'bin/x86_64-w64-mingw32-clang'
    resource=Path(subprocess.check_output([str(driver),'-print-resource-dir'],text=True).strip()).resolve()
    assert resource.is_relative_to(llvm)
    roots=[target/'lib',target/'bin']
    assert all(root.is_dir() for root in roots)
    if (resource/'lib').is_dir(): roots.append(resource/'lib')
    files=sorted({p.resolve() for root in roots for p in root.rglob('*')
                  if p.is_file() and (p.name.endswith('.a') or p.name.endswith('.o') or p.name.lower().endswith('.dll'))})
    assert files and all(p.is_relative_to(llvm) for p in files)
    nm=llvm/'bin/llvm-nm'; readobj=llvm/'bin/llvm-readobj'; rows=[]
    for path in files:
        argv=([str(readobj),'--coff-imports',str(path)] if path.name.lower().endswith('.dll')
              else [str(nm),'--undefined-only','--print-file-name',str(path)])
        result=subprocess.run(argv,capture_output=True,text=True)
        if result.returncode: raise RuntimeError('Runtime symbol inventory failed: '+str(path)+' '+result.stderr[:400])
        hits=[line.strip() for line in result.stdout.splitlines() if any(symbol.casefold() in line.casefold() for symbol in SYMBOLS)]
        rows.append({'path':str(path.relative_to(llvm)),'sha256':sha(path),'command':argv,'hits':hits})
    data={'role':'pinned-toolchain-runtime-symbol-inventory-v1','GITHUB_SHA':os.environ['GITHUB_SHA'],
          'GITHUB_RUN_ID':os.environ['GITHUB_RUN_ID'],'GITHUB_RUN_ATTEMPT':os.environ['GITHUB_RUN_ATTEMPT'],
          'driver_sha256':sha(driver),'nm_sha256':sha(nm),'readobj_sha256':sha(readobj),
          'resource':str(resource.relative_to(llvm)),'roots':[str(root.relative_to(llvm)) for root in roots],'symbols':list(SYMBOLS),'rows':rows}
    output.write_text(json.dumps(data,sort_keys=True,indent=2))
    print('RUNTIME_SYMBOL_INVENTORY files='+str(len(rows))+' matched_files='+str(sum(bool(row['hits']) for row in rows)))
    for row in rows:
        if row['hits']: print('RUNTIME_SYMBOL_MATCH '+row['path']+' '+repr(row['hits']))
if __name__=='__main__': run(Path(sys.argv[1]),Path(sys.argv[2]))
