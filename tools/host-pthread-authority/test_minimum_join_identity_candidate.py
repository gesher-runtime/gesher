"""Pure negative controls; no network, CI, compiler or native processes."""
import copy
import importlib.util
from pathlib import Path
spec = importlib.util.spec_from_file_location("candidate", Path(__file__).with_name("minimum_join_identity_candidate.py"))
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)

def reject(fn):
    try: fn()
    except (ValueError, TypeError): return
    raise AssertionError('invalid identity accepted')

def main():
    source='1'*40; guard='2'*40
    env=dict(GITHUB_ACTIONS='true', GITHUB_REPOSITORY=a.REPOSITORY, GITHUB_REPOSITORY_ID=str(a.REPOSITORY_ID),
      GITHUB_REF='refs/heads/'+a.BRANCH, GITHUB_WORKFLOW=a.WORKFLOW,
      GITHUB_WORKFLOW_REF=a.REPOSITORY+'/'+a.CALLER+'@refs/heads/'+a.BRANCH, GITHUB_JOB='diagnostic-producer',
      GITHUB_ACTOR='palmoni5', GITHUB_TRIGGERING_ACTOR='palmoni5', GITHUB_SHA=source,
      GITHUB_WORKFLOW_SHA=source, GITHUB_EVENT_NAME='push', GITHUB_RUN_ID='123', GITHUB_RUN_ATTEMPT='1',
      **{a.VARIABLE:source})
    ctx=a.verify_context(env,'diagnostic-producer'); count=0
    for key in env:
        bad=dict(env);bad[key]='invalid';reject(lambda:a.verify_context(bad,'diagnostic-producer'));count+=1
    reject(lambda:a.verify_context(env,'unknown'));count+=1
    run=dict(id=123,run_attempt=1,head_sha=source,head_branch=a.BRANCH,path=a.CALLER,name=a.WORKFLOW,event='push',
      repository=dict(id=a.REPOSITORY_ID,full_name=a.REPOSITORY),head_repository=dict(id=a.REPOSITORY_ID,full_name=a.REPOSITORY),
      actor=dict(login='palmoni5'),triggering_actor=dict(login='palmoni5'),
      referenced_workflows=[dict(path=a.REPOSITORY+'/'+a.GUARD+'@'+guard,sha=guard)])
    a.verify_run(run,ctx,guard)
    for key in run:
        bad=copy.deepcopy(run);bad[key]=None;reject(lambda:a.verify_run(bad,ctx,guard));count+=1
    producer=dict(name='diagnostic-producer',head_sha=source,run_id=123,run_attempt=1,status='completed',conclusion='success',id=42)
    assert a.verify_producer([producer],ctx)==42
    for key in producer:
        bad=copy.deepcopy(producer);bad[key]=None;reject(lambda:a.verify_producer([bad],ctx));count+=1
    reject(lambda:a.verify_producer([producer,producer],ctx));count+=1
    print('IDENTITY_PURE_PASS negatives='+str(count)+' actual_authority=false native_network=0')
if __name__=='__main__':main()
