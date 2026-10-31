"""Pure negative tests only; not an immutable source gate or CI acceptance."""
import copy, importlib.util
from pathlib import Path
spec=importlib.util.spec_from_file_location('candidate',Path(__file__).with_name('minimum_join_runtime_authority_wip.py'))
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
def reject(fn):
    try:fn()
    except (ValueError,KeyError,TypeError,AttributeError):return
    raise AssertionError('accepted invalid current-run role')
def main():
    source='1'*40;guard='2'*40;ref=a.REPO+'/'+a.GUARD+'@'+guard
    env={'GITHUB_ACTIONS':'true','GITHUB_REPOSITORY':a.REPO,'GITHUB_REPOSITORY_ID':str(a.REPO_ID),
      'GITHUB_REF':'refs/heads/'+a.BRANCH,'GITHUB_WORKFLOW':a.WORKFLOW,'GITHUB_WORKFLOW_REF':a.REPO+'/'+a.CALLER+'@refs/heads/'+a.BRANCH,
      'GITHUB_JOB':'diagnostic-producer','GITHUB_ACTOR':'palmoni5','GITHUB_TRIGGERING_ACTOR':'palmoni5',
      'GITHUB_SHA':source,'GITHUB_WORKFLOW_SHA':source,a.VARIABLE:source,'GESHER_MINIMUM_JOIN_RUNTIME_AUTHORIZED_SOURCE':source,
      'GESHER_MINIMUM_JOIN_RUNTIME_POLICY_REF':ref,'GITHUB_EVENT_NAME':'push','GITHUB_RUN_ID':'123','GITHUB_RUN_ATTEMPT':'1'}
    c=a.context(env);count=0
    for key in env:
        bad=dict(env);bad[key]='bad';reject(lambda:a.context(bad));count+=1
    run={'id':123,'run_attempt':1,'head_sha':source,'head_branch':a.BRANCH,'path':a.CALLER,'name':a.WORKFLOW,'event':'push',
      'repository':{'id':a.REPO_ID,'full_name':a.REPO},'head_repository':{'id':a.REPO_ID,'full_name':a.REPO},
      'actor':{'login':'palmoni5'},'triggering_actor':{'login':'palmoni5'},'referenced_workflows':[{'path':ref,'sha':guard}]}
    a.check_run(run,c)
    for key in run:
        bad=copy.deepcopy(run);bad[key]=None;reject(lambda:a.check_run(bad,c));count+=1
    def job(name,id,status,conclusion):return {'name':name,'id':id,'head_sha':source,'run_id':123,'run_attempt':1,'status':status,'conclusion':conclusion}
    jobs={'total_count':2,'jobs':[job('source-authority / verify',11,'completed','success'),job('diagnostic-producer',12,'in_progress',None)]}
    assert a.check_jobs(jobs,c)=={'source-authority / verify':11,'diagnostic-producer':12}
    for i in range(2):
        for key in ('id','head_sha','run_id','run_attempt','status'):
            bad=copy.deepcopy(jobs);bad['jobs'][i][key]=None;reject(lambda:a.check_jobs(bad,c));count+=1
    bad=copy.deepcopy(jobs);bad['jobs'].append(copy.deepcopy(bad['jobs'][1]));bad['total_count']=3;reject(lambda:a.check_jobs(bad,c));count+=1
    bad=copy.deepcopy(jobs);bad['jobs'][0]['id']=12;reject(lambda:a.check_jobs(bad,c));count+=1
    consumer_env=dict(env,GITHUB_JOB='diagnostic-windows',JOIN_RUNTIME_OS='windows-2025',JOIN_RUNTIME_MODE='dynamic')
    consumer=a.context(consumer_env,'diagnostic-windows')
    consumer_jobs={'total_count':3,'jobs':[job('source-authority / verify',11,'completed','success'),job('diagnostic-producer',12,'completed','success'),job('diagnostic-windows-windows-2025-dynamic',13,'in_progress',None)]}
    assert a.check_jobs(consumer_jobs,consumer)['diagnostic-producer']==12
    for index,key in ((0,'conclusion'),(1,'conclusion'),(1,'run_attempt'),(2,'head_sha'),(2,'status'),(2,'id')):
        bad=copy.deepcopy(consumer_jobs);bad['jobs'][index][key]=None;reject(lambda:a.check_jobs(bad,consumer));count+=1
    for key in ('JOIN_RUNTIME_OS','JOIN_RUNTIME_MODE'):
        bad=dict(consumer_env);bad[key]='bad';reject(lambda:a.context(bad,'diagnostic-windows'));count+=1
    print('JOIN_RUNTIME_AUTHORITY_PURE_PASS negatives='+str(count)+' immutable_gate=false native_network=0')
if __name__=='__main__':main()
