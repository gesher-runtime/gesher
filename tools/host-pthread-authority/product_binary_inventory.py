"""CI-only read-only scan of exact qualified 409110 product artifact bytes."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

RUN='40911022043'; ATTEMPT=1; SOURCE='7e5ab67afd841c842c6c5c278a84f7e2512e0ca9'
ROLES={
 'windows-x64':(12915354209,'sha256:156ac159b1cfdbaf9adc7cb405af612658d8cd0d06b887fa35650aa1367b03f8','build',123578880128),
 'arm64':(12914860307,'sha256:2285b29eb92e744f609124cacc3fbff085264c16126d78a4fa4b6cfa125dd484','arm64 / build',123580076664),
 'webview':(12914878441,'sha256:92c11d6118367ac1101516ddd703879da3ebfeab94a934fc265e4871b9ec0102','webview / build',123592743829),
 'angle':(12913892245,'sha256:2cf20d4ad4f567a61178c115159967f204ee4a96fc4c66c366565107d84f5204','angle / build',123578882780),
 'graphics':(12915320136,'sha256:635586b473ca6dec774db88a10737865dbe5e44fc3cbca8539988cd8f271f41e','graphics / build',123578882416),
 'media':(12914999234,'sha256:33db39b3a60132f490be1610d9656024cfdfb0c89815fab96a17f36b2f795ee7','media / media',123592743510),
}
SYMBOLS=('_pthread_key_dest','pthread_getclean','pthread_self','pthread_create','__pth_gpointer_locked',
         'gesher_clock_anchor_v1','gesher_winpthread_dup_cpu_handle_v1','libwinpthread')
ROOT=Path(__file__).resolve().parents[2]
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def api(path): return json.loads(subprocess.check_output(['gh','api','repos/gesher-runtime/gesher/'+path]))
def timestamp(value): return datetime.datetime.fromisoformat(value.replace('Z','+00:00'))
def main(llvm):
    assert os.environ['GITHUB_ACTIONS']=='true' and os.environ['GITHUB_REPOSITORY']=='gesher-runtime/gesher'
    assert os.environ['GITHUB_REF']=='refs/heads/wip/pthread-product-binary-map'
    assert subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()==os.environ['GITHUB_SHA']
    subprocess.check_call(['git','diff','--exit-code','HEAD','--'])
    repo_id=int(os.environ['GITHUB_REPOSITORY_ID']); run=api('actions/runs/'+RUN)
    assert str(run['id'])==RUN and run['run_attempt']==ATTEMPT and run['head_sha']==SOURCE
    assert run['status']=='completed' and run['conclusion']=='success' and run['repository']['id']==repo_id and run['head_repository']['id']==repo_id
    jobs=api('actions/runs/'+RUN+'/attempts/1/jobs?per_page=100'); artifacts=api('actions/runs/'+RUN+'/artifacts?per_page=100')
    assert jobs['total_count']<=100 and artifacts['total_count']<=100
    llvm=Path(llvm).resolve(); nm=llvm/'bin/llvm-nm'; readobj=llvm/'bin/llvm-readobj'; out=ROOT/'product-binary-inventory'; out.mkdir()
    supplier_path=llvm/'gesher-winpthreads-authority.json'; supplier=json.loads(supplier_path.read_text())
    assert supplier['role']=='gesher-winpthreads-authority-v1'
    assert all(supplier[k]==os.environ[k] for k in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY'))
    data={'role':'qualified-409110-product-binary-inventory-v1','source_run':RUN,'source_attempt':ATTEMPT,'source_sha':SOURCE,
          'scan_sha':os.environ['GITHUB_SHA'],'scan_run':os.environ['GITHUB_RUN_ID'],'scan_attempt':os.environ['GITHUB_RUN_ATTEMPT'],
          'nm_sha256':sha(nm),'readobj_sha256':sha(readobj),'scanner_supplier_receipt_sha256':sha(supplier_path),
          'compiler_archive_sha256':supplier['compiler_archive_sha256'],'driver_sha256':supplier['compiler_driver_sha256'],'symbols':list(SYMBOLS),'roles':{}}
    for name,(artifact_id,digest,job_name,job_id) in ROLES.items():
        found=[a for a in artifacts['artifacts'] if a['name']==name]; matches=[j for j in jobs['jobs'] if j['name']==job_name]
        assert len(found)==1 and len(matches)==1
        a=found[0]; j=matches[0]
        assert a['id']==artifact_id and a['digest']==digest and not a['expired'] and a['size_in_bytes']>0
        assert a['workflow_run']['id']==int(RUN) and a['workflow_run']['head_sha']==SOURCE
        assert a['workflow_run']['repository_id']==repo_id and a['workflow_run']['head_repository_id']==repo_id
        assert j['id']==job_id and j['run_id']==int(RUN) and j['run_attempt']==ATTEMPT and j['head_sha']==SOURCE and j['conclusion']=='success'
        assert timestamp(j['started_at'])<=timestamp(a['created_at'])<=timestamp(j['completed_at'])
        dest=Path(os.environ['RUNNER_TEMP'])/('gesher-product-inventory-'+name)
        assert not dest.exists()
        subprocess.check_call([sys.executable,str(ROOT/'tools/surface/download-verified-artifact.py'),
                               '--id',str(artifact_id),'--digest',digest,'--destination',str(dest)])
        rows=[]; nested=[]; other=[]; unclassified=[]
        for path in sorted(p for p in dest.rglob('*') if p.is_file()):
            rel=str(path.relative_to(dest)); lower=path.name.lower(); magic=path.open('rb').read(8)
            if lower.endswith(('.zip','.jar','.aar','.apk')): nested.append({'path':rel,'sha256':sha(path)}); continue
            if magic.startswith(b'\x7fELF'): other.append({'path':rel,'kind':'ELF','sha256':sha(path)}); continue
            if magic.startswith(b'MZ'):
                commands=[[str(readobj),'--coff-imports','--coff-exports',str(path)],[str(nm),'--defined-only','--print-file-name',str(path)]]; kind='PE'
            elif lower.endswith(('.a','.lib','.o','.obj')):
                commands=[[str(readobj),'--file-headers',str(path)],[str(nm),'--defined-only','--print-file-name',str(path)],[str(nm),'--undefined-only','--print-file-name',str(path)]]; kind='archive-or-object-abi-not-inferred'
            else: unclassified.append({'path':rel,'sha256':sha(path)}); continue
            results=[]
            for argv in commands:
                r=subprocess.run(argv,capture_output=True,text=True,errors='replace')
                hits=[line.strip() for line in r.stdout.splitlines() if any(symbol.casefold() in line.casefold() for symbol in SYMBOLS)]
                formats=[line.strip() for line in r.stdout.splitlines() if 'Format:' in line or 'Machine:' in line]
                results.append({'argv':argv,'exit':r.returncode,'stderr':r.stderr[:1000],'hits':hits,'formats':formats})
            rows.append({'path':rel,'kind':kind,'sha256':sha(path),'results':results})
        assert rows or nested or other or unclassified, 'Empty role '+name
        data['roles'][name]={'artifact_id':artifact_id,'artifact_digest':digest,'producer_id':job_id,'producer':job_name,
                             'rows':rows,'elf_files':other,'nested_archives':nested,'unclassified_files':unclassified}
        print('PRODUCT_BINARY_INVENTORY role='+name+' host_rows='+str(len(rows))+' elf='+str(len(other))+' nested='+str(len(nested))+' unclassified='+str(len(unclassified))+' errors='+str(sum(r['exit']!=0 for row in rows for r in row['results'])),flush=True)
        for row in rows:
            for result in row['results']:
                if result['hits']: print('PRODUCT_BINARY_MATCH role='+name+' path='+row['path']+' hits='+repr(result['hits']),flush=True)
    (out/'inventory.json').write_text(json.dumps(data,sort_keys=True,indent=2))
    print('PRODUCT_BINARY_INVENTORY source='+SOURCE+' run='+RUN+' attempt=1; read-only, no product qualification',flush=True)
if __name__=='__main__': main(sys.argv[1])
