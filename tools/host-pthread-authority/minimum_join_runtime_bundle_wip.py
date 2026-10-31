"""WIP exact-current diagnostic artifact seal; no target-wait qualification.

Only a separately reviewed immutable workflow and current artifact receipt may
call these routines. This file alone cannot authorize or publish a producer.
"""
import hashlib, importlib.util, json, os, sys
from pathlib import Path

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
import minimum_join_runtime_authority_wip as authority
spec=importlib.util.spec_from_file_location('join_linkage',HERE/'verify-minimum-join-linkage-wip.py')
linkage=importlib.util.module_from_spec(spec);spec.loader.exec_module(linkage)
PAYLOAD={'candidate.obj','candidate-undefined.txt','static.exe','dynamic.exe',
 'static.map','dynamic.map','static-link.txt','dynamic-link.txt','static-imports.txt','dynamic-imports.txt',
 'static-symbols.txt','dynamic-symbols.txt','static-disassembly.txt','dynamic-disassembly.txt',
 'libwinpthread-1.dll','dll-exports.txt','dll-disassembly.txt','libwinpthread.a','libwinpthread.dll.a',
 'import-archive-symbols.txt','producer-archive-observations.json','link-proof.json'}
CAP=128*1024*1024

def digest(data):return hashlib.sha256(data).hexdigest()
def scan(root,names):
    assert root.is_dir() and not root.is_symlink() and {p.name for p in root.iterdir()}==set(names)
    result={};total=0
    for name in sorted(names):
        path=root/name;assert not path.is_symlink() and path.is_file()
        data=path.read_bytes();assert 0<len(data)<=CAP;total+=len(data);assert total<=CAP
        result[name]={'bytes':len(data),'sha256':digest(data)}
    return result

def seal(root,auth):
    root=Path(root);c=auth['context'];assert c['role']=='diagnostic-producer'
    assert {p.name for p in root.iterdir()}==PAYLOAD-{'link-proof.json'}
    ctx={'source':c['source'],'run':str(c['run']),'attempt':str(c['attempt'])}
    proof=linkage.verify(root,ctx)
    assert proof['target_wait_proved'] is False and proof['product_qualification'] is False and proof['sealed'] is False
    (root/'link-proof.json').write_text(json.dumps(proof,sort_keys=True,indent=2)+'\n',encoding='utf-8')
    members=scan(root,PAYLOAD)
    manifest={'schema':1,'source':c['source'],'run':c['run'],'attempt':c['attempt'],'guard':c['guard'],
      'producer_id':auth['roles']['diagnostic-producer'],'role':'join-wait-diagnostic-only-v1','members':members,
      'target_wait_proved':False,'product_qualification':False}
    (root/'producer-manifest.json').write_text(json.dumps(manifest,sort_keys=True,indent=2)+'\n',encoding='utf-8')
    return manifest

def verify(root,auth):
    root=Path(root);c=auth['context'];assert c['role']=='diagnostic-windows'
    members=scan(root,PAYLOAD|{'producer-manifest.json'});manifest=json.loads((root/'producer-manifest.json').read_text(encoding='utf-8'))
    assert set(manifest)=={'schema','source','run','attempt','guard','producer_id','role','members','target_wait_proved','product_qualification'}
    assert manifest['schema']==1 and manifest['source']==c['source'] and manifest['run']==c['run'] and manifest['attempt']==c['attempt'] and manifest['guard']==c['guard']
    assert manifest['producer_id']==auth['roles']['diagnostic-producer'] and manifest['role']=='join-wait-diagnostic-only-v1'
    assert manifest['target_wait_proved'] is False and manifest['product_qualification'] is False
    assert manifest['members']=={p:members[p] for p in PAYLOAD}
    proof=linkage.verify(root,{'source':c['source'],'run':str(c['run']),'attempt':str(c['attempt'])})
    assert proof==json.loads((root/'link-proof.json').read_text(encoding='utf-8'))
    return manifest

if __name__=='__main__':
    import sys
    mode=sys.argv[1];assert mode in ('seal','verify') and len(sys.argv)==3
    auth=authority.authorize('diagnostic-producer' if mode=='seal' else 'diagnostic-windows')
    result=seal(sys.argv[2],auth) if mode=='seal' else verify(sys.argv[2],auth)
    print('JOIN_RUNTIME_DIAGNOSTIC_'+mode.upper()+' '+json.dumps(result,sort_keys=True),flush=True)
