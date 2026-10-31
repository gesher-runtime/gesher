"""WIP current-run authority for isolated diagnostic producer, not a source gate.

Call only after a separately frozen immutable gate has bound this exact file.
No product/default selection or target-wait qualification follows.
"""
import json, os, re, subprocess

REPO='gesher-runtime/gesher'; REPO_ID=1426153186
BRANCH='wip/pthread-minimum-join-witness-runtime'
WORKFLOW='pthread-minimum-join-runtime-component'
CALLER='.github/workflows/pthread-minimum-join-runtime-component.yml'
GUARD='.github/workflows/pthread-minimum-join-runtime-trusted-source.yml'
VARIABLE='PTHREAD_MINIMUM_JOIN_RUNTIME_CALLER_COMMIT'

def require(value,why):
    if not value:raise ValueError(why)

def context(env):
    expected={'GITHUB_ACTIONS':'true','GITHUB_REPOSITORY':REPO,'GITHUB_REPOSITORY_ID':str(REPO_ID),
      'GITHUB_REF':'refs/heads/'+BRANCH,'GITHUB_WORKFLOW':WORKFLOW,
      'GITHUB_WORKFLOW_REF':REPO+'/'+CALLER+'@refs/heads/'+BRANCH,
      'GITHUB_JOB':'diagnostic-producer','GITHUB_ACTOR':'palmoni5','GITHUB_TRIGGERING_ACTOR':'palmoni5'}
    require(all(env.get(k)==v for k,v in expected.items()),'exact producer context')
    source=env.get('GITHUB_SHA','');guard=env.get('GESHER_MINIMUM_JOIN_RUNTIME_POLICY_REF','')
    require(re.fullmatch('[0-9a-f]{40}',source) is not None,'source')
    require(source==env.get('GITHUB_WORKFLOW_SHA')==env.get(VARIABLE)==env.get('GESHER_MINIMUM_JOIN_RUNTIME_AUTHORIZED_SOURCE'),'current approved source')
    require(re.fullmatch(re.escape(REPO+'/'+GUARD+'@')+'[0-9a-f]{40}',guard) is not None,'immutable guard reference')
    require(env.get('GITHUB_EVENT_NAME') in ('push','workflow_dispatch'),'event')
    for key in ('GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'):require(re.fullmatch('[1-9][0-9]*',env.get(key,'')) is not None,key)
    return {'source':source,'run':int(env['GITHUB_RUN_ID']),'attempt':int(env['GITHUB_RUN_ATTEMPT']),'guard':guard}

def check_run(run,c):
    expected={'id':c['run'],'run_attempt':c['attempt'],'head_sha':c['source'],'head_branch':BRANCH,'path':CALLER,'name':WORKFLOW}
    require(all(run.get(k)==v for k,v in expected.items()) and run.get('event') in ('push','workflow_dispatch'),'live run identity')
    for k in ('repository','head_repository'):
        require(type(run.get(k)) is dict and run[k].get('id')==REPO_ID and run[k].get('full_name')==REPO,'live repository')
    for k in ('actor','triggering_actor'):require(type(run.get(k)) is dict and run[k].get('login')=='palmoni5','live actor')
    refs=run.get('referenced_workflows');require(type(refs) is list and len(refs)==1 and refs[0].get('path')==c['guard'] and refs[0].get('sha')==c['guard'].rsplit('@',1)[1],'live called guard')

def check_jobs(data,c):
    rows=data.get('jobs');require(type(rows) is list and data.get('total_count')==len(rows)<=100,'complete current jobs')
    required={'source-authority / verify':'success','diagnostic-producer':None};selected={}
    for name,conclusion in required.items():
        found=[r for r in rows if r.get('name')==name];require(len(found)==1,'unique '+name);row=found[0]
        require(row.get('head_sha')==c['source'] and row.get('run_id')==c['run'] and row.get('run_attempt')==c['attempt'],'job source attempt')
        require(type(row.get('id')) is int and row['id']>0,'job id')
        if conclusion:require(row.get('status')=='completed' and row.get('conclusion')==conclusion,'source authority completed')
        else:require(row.get('status')=='in_progress' and row.get('conclusion') is None,'producer active')
        selected[name]=row['id']
    require(len(set(selected.values()))==2,'distinct producer/guard');return selected

def main():
    c=context(os.environ)
    run=json.loads(subprocess.check_output(['gh','api','repos/'+REPO+'/actions/runs/'+str(c['run'])]))
    jobs=json.loads(subprocess.check_output(['gh','api','repos/'+REPO+'/actions/runs/'+str(c['run'])+'/attempts/'+str(c['attempt'])+'/jobs?per_page=100']))
    check_run(run,c);roles=check_jobs(jobs,c)
    print('JOIN_RUNTIME_CURRENT_PRODUCER '+json.dumps({'context':c,'roles':roles,'qualification':False},sort_keys=True),flush=True)

if __name__=='__main__':main()
