"""Immutable-bootstrap source gate for diagnostic object collection only."""
import hashlib, json, os, re, subprocess
from pathlib import Path

BASE='f0a8364afd3380eb0e25b8d82824dd1752f91e16'
REPO='gesher-runtime/gesher'; RID='1426153186'
BRANCH='wip/pthread-minimum-join-nontail-object'
NAME='pthread-minimum-join-cancel-component'
CALLER='.github/workflows/pthread-minimum-join-cancel-component.yml'
GUARD='.github/workflows/pthread-minimum-join-cancel-trusted-source.yml'
VAR='PTHREAD_MINIMUM_JOIN_NONTAIL_CALLER_COMMIT'
ADDITIONS={CALLER,GUARD,
 'docs/handoff/tracks/pthread-minimum-join-cancellation-candidate.md',
 'tests/native/host_pthread_minimum_join_cancel_candidate.c',
 'tools/host-pthread-authority/audit-minimum-join-witness-source.py',
 'tools/host-pthread-authority/collect-minimum-join-object-wip.sh',
 'tools/host-pthread-authority/minimum-join-wait-diagnostic.patch',
 'tools/host-pthread-authority/minimum_join_identity_candidate.py',
 'tools/host-pthread-authority/run-minimum-join-cancel-candidate.ps1',
 'tools/host-pthread-authority/test_minimum_join_identity_candidate.py',
 'tools/host-pthread-authority/minimum_join_collector_policy.py'}

def git(*args):
    return subprocess.check_output(['git',*args])

def blob(raw):
    return hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()

def inventory(ref):
    rows={}
    for item in git('ls-tree','-rz','--full-tree',ref).split(b'\0'):
        if not item: continue
        meta,path=item.split(b'\t',1);mode,kind,oid=meta.decode().split(' ');name=path.decode('utf-8')
        assert kind=='blob' and mode in ('100644','100755') and name not in rows
        rows[name]=(mode,oid)
    return rows

def normalize(raw):
    pattern=rb'(uses: gesher-runtime/gesher/\.github/workflows/pthread-minimum-join-cancel-trusted-source\.yml@)[0-9a-f]{40}'
    result,count=re.subn(pattern,lambda m:m.group(1)+b'0'*40,raw)
    assert count==1
    return result

def verify_source(source,guard,pins):
    assert re.fullmatch('[0-9a-f]{40}',source) and re.fullmatch('[0-9a-f]{40}',guard)
    assert set(pins)==ADDITIONS-{GUARD}
    assert git('rev-parse','HEAD').decode().strip()==source
    assert subprocess.run(['git','merge-base','--is-ancestor',BASE,source]).returncode==0
    base=inventory(BASE);current=inventory(source)
    assert len(base)==1820 and not (set(base)&ADDITIONS) and set(current)==set(base)|ADDITIONS
    assert all(current[p]==value for p,value in base.items())
    for path in ADDITIONS-{GUARD}:
        raw=git('show',source+':'+path)
        if path==CALLER: raw=normalize(raw)
        assert current[path][0]=='100644' and blob(raw)==pins[path]
    caller=git('show',source+':'+CALLER)
    assert caller.count((GUARD+'@'+guard).encode())==1
    assert git('show',source+':'+GUARD)==git('show',guard+':'+GUARD)
    assert current[GUARD]==('100644',git('rev-parse',guard+':'+GUARD).decode().strip())
    assert not git('status','--porcelain','--untracked-files=all').strip()
    names=sorted(current)
    actual=subprocess.check_output(['git','hash-object','--stdin-paths'],input=('\n'.join(names)+'\n').encode()).decode().splitlines()
    assert len(actual)==len(names) and all(current[p][1]==v for p,v in zip(names,actual))
    return len(current)

def gate(pins):
    e=os.environ; source=e['GITHUB_SHA']
    assert re.fullmatch('[0-9a-f]{40}', source)
    caller=git('show',source+':'+CALLER)
    matches=re.findall(rb'pthread-minimum-join-cancel-trusted-source\.yml@([0-9a-f]{40})',caller)
    assert len(matches)==1
    guard=matches[0].decode()
    expected={'GITHUB_ACTIONS':'true','GITHUB_REPOSITORY':REPO,'GITHUB_REPOSITORY_ID':RID,
      'GITHUB_REF':'refs/heads/'+BRANCH,'GITHUB_WORKFLOW':NAME,
      'GITHUB_WORKFLOW_REF':REPO+'/'+CALLER+'@refs/heads/'+BRANCH,'GITHUB_JOB':'verify',
      'GITHUB_ACTOR':'palmoni5','GITHUB_TRIGGERING_ACTOR':'palmoni5','GITHUB_EVENT_NAME':'push',
      'GITHUB_WORKFLOW_SHA':source,VAR:source}
    assert all(e.get(k)==v for k,v in expected.items())
    for k in ('GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'):assert re.fullmatch('[1-9][0-9]*',e[k])
    assert re.fullmatch('[0-9a-f]{40}',source) and re.fullmatch('[0-9a-f]{40}',guard)
    run=json.loads(subprocess.check_output(['gh','api','repos/'+REPO+'/actions/runs/'+e['GITHUB_RUN_ID']]))
    assert run['id']==int(e['GITHUB_RUN_ID']) and run['run_attempt']==int(e['GITHUB_RUN_ATTEMPT'])
    assert run['head_sha']==source and run['head_branch']==BRANCH and run['path']==CALLER and run['name']==NAME and run['event']=='push'
    for k in ('repository','head_repository'):assert run[k]['id']==int(RID) and run[k]['full_name']==REPO
    for k in ('actor','triggering_actor'):assert run[k]['login']=='palmoni5'
    refs=run['referenced_workflows'];assert len(refs)==1 and refs[0]['path']==REPO+'/'+GUARD+'@'+guard and refs[0]['sha']==guard
    count=verify_source(source,guard,pins)
    with open(e['GITHUB_OUTPUT'],'a',encoding='utf-8',newline='\n') as f:f.write('source='+source+'\n')
    print('JOIN_OBJECT_SOURCE_PASS source='+source+' guard='+guard+' files='+str(count)+' runtime=false qualification=false',flush=True)
