"""WIP current-run authority for isolated diagnostic producer, not a source gate.

Call only after a separately frozen immutable gate has bound this exact file.
No product/default selection or target-wait qualification follows.
"""
import json, os, re, subprocess, sys

REPO='gesher-runtime/gesher'; REPO_ID=1426153186
BRANCH='wip/pthread-minimum-join-witness-runtime'
WORKFLOW='pthread-minimum-join-runtime-component'
CALLER='.github/workflows/pthread-minimum-join-runtime-component.yml'
GUARD='.github/workflows/pthread-minimum-join-runtime-trusted-source.yml'
VARIABLE='PTHREAD_MINIMUM_JOIN_RUNTIME_CALLER_COMMIT'

def require(value,why):
    if not value:raise ValueError(why)

def context(env, role='diagnostic-producer'):
    require(role in ('diagnostic-producer','diagnostic-windows'),'unsupported role')
    expected={'GITHUB_ACTIONS':'true','GITHUB_REPOSITORY':REPO,'GITHUB_REPOSITORY_ID':str(REPO_ID),
      'GITHUB_REF':'refs/heads/'+BRANCH,'GITHUB_WORKFLOW':WORKFLOW,
      'GITHUB_WORKFLOW_REF':REPO+'/'+CALLER+'@refs/heads/'+BRANCH,
      'GITHUB_JOB':role,'GITHUB_ACTOR':'palmoni5','GITHUB_TRIGGERING_ACTOR':'palmoni5'}
    require(all(env.get(k)==v for k,v in expected.items()),'exact producer context')
    source=env.get('GITHUB_SHA','');guard=env.get('GESHER_MINIMUM_JOIN_RUNTIME_POLICY_REF','')
    require(re.fullmatch('[0-9a-f]{40}',source) is not None,'source')
    require(source==env.get('GITHUB_WORKFLOW_SHA')==env.get(VARIABLE)==env.get('GESHER_MINIMUM_JOIN_RUNTIME_AUTHORIZED_SOURCE'),'current approved source')
    require(re.fullmatch(re.escape(REPO+'/'+GUARD+'@')+'[0-9a-f]{40}',guard) is not None,'immutable guard reference')
    require(env.get('GITHUB_EVENT_NAME') in ('push','workflow_dispatch'),'event')
    for key in ('GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'):require(re.fullmatch('[1-9][0-9]*',env.get(key,'')) is not None,key)
    result={'source':source,'run':int(env['GITHUB_RUN_ID']),'attempt':int(env['GITHUB_RUN_ATTEMPT']),'guard':guard,'role':role}
    if role=='diagnostic-windows':
        require(env.get('JOIN_RUNTIME_OS') in ('windows-2022','windows-2025') and env.get('JOIN_RUNTIME_MODE') in ('static','dynamic'),'exact consumer matrix')
        result['consumer_name']='diagnostic-windows-'+env['JOIN_RUNTIME_OS']+'-'+env['JOIN_RUNTIME_MODE']
    return result

def check_run(run,c):
    expected={'id':c['run'],'run_attempt':c['attempt'],'head_sha':c['source'],'head_branch':BRANCH,'path':CALLER,'name':WORKFLOW}
    require(all(run.get(k)==v for k,v in expected.items()) and run.get('event') in ('push','workflow_dispatch'),'live run identity')
    for k in ('repository','head_repository'):
        require(type(run.get(k)) is dict and run[k].get('id')==REPO_ID and run[k].get('full_name')==REPO,'live repository')
    for k in ('actor','triggering_actor'):require(type(run.get(k)) is dict and run[k].get('login')=='palmoni5','live actor')
    refs=run.get('referenced_workflows');require(type(refs) is list and len(refs)==1 and refs[0].get('path')==c['guard'] and refs[0].get('sha')==c['guard'].rsplit('@',1)[1],'live called guard')

def check_jobs(data,c):
    rows=data.get('jobs');require(type(rows) is list and data.get('total_count')==len(rows)<=100,'complete current jobs')
    required={'source-authority / verify':'success','diagnostic-producer':'success' if c['role']=='diagnostic-windows' else None};selected={}
    for name,conclusion in required.items():
        found=[r for r in rows if r.get('name')==name];require(len(found)==1,'unique '+name);row=found[0]
        require(row.get('head_sha')==c['source'] and row.get('run_id')==c['run'] and row.get('run_attempt')==c['attempt'],'job source attempt')
        require(type(row.get('id')) is int and row['id']>0,'job id')
        if conclusion:require(row.get('status')=='completed' and row.get('conclusion')==conclusion,'source authority completed')
        else:require(row.get('status')=='in_progress' and row.get('conclusion') is None,'producer active')
        selected[name]=row['id']
    if c['role']=='diagnostic-windows':
        name=c['consumer_name']; found=[r for r in rows if r.get('name')==name]
        require(len(found)==1,'unique live consumer');row=found[0]
        require(row.get('head_sha')==c['source'] and row.get('run_id')==c['run'] and row.get('run_attempt')==c['attempt'] and row.get('status')=='in_progress' and row.get('conclusion') is None and type(row.get('id')) is int and row['id']>0,'current live consumer')
        selected[name]=row['id']
    require(len(set(selected.values()))==len(selected),'distinct role IDs');return selected

def authorize(role='diagnostic-producer'):
    c=context(os.environ,role)
    run=json.loads(subprocess.check_output(['gh','api','repos/'+REPO+'/actions/runs/'+str(c['run'])]))
    jobs=json.loads(subprocess.check_output(['gh','api','repos/'+REPO+'/actions/runs/'+str(c['run'])+'/attempts/'+str(c['attempt'])+'/jobs?per_page=100']))
    check_run(run,c);roles=check_jobs(jobs,c)
    return {'context':c,'roles':roles,'qualification':False}

def main():
    require(len(sys.argv)==2 and sys.argv[1] in ('diagnostic-producer','diagnostic-windows'),'explicit role argument')
    result=authorize(sys.argv[1])
    print('JOIN_RUNTIME_CURRENT_ROLE '+json.dumps(result,sort_keys=True),flush=True)

if __name__=='__main__':main()
