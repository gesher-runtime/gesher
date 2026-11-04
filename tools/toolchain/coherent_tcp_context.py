"""Exact selected-only current-source TCP roles within coherent full CI."""
import re

REPOSITORY='gesher-runtime/gesher'; REPOSITORY_ID='1426153186'
BRANCH='wip/coherent-selected-tcp'; WORKFLOW='ci'
PATH='.github/workflows/ci.yml'

def context(env,job):
    assert job in ('compile','linux-reference','windows','supplier-correlation') and env.get('GITHUB_JOB')==job
    assert env.get('GITHUB_REF') in ('refs/heads/'+BRANCH, 'refs/heads/main')
    assert env.get('GITHUB_EVENT_NAME') in ('workflow_dispatch', 'push')
    assert env.get('GITHUB_WORKFLOW_REF') in (REPOSITORY+'/'+PATH+'@refs/heads/'+BRANCH, REPOSITORY+'/'+PATH+'@refs/heads/main')
    expected={'GITHUB_ACTIONS':'true','GITHUB_REPOSITORY':REPOSITORY,'GITHUB_REPOSITORY_ID':REPOSITORY_ID,
              'GITHUB_WORKFLOW':WORKFLOW,
              'GITHUB_ACTOR':'palmoni5','GITHUB_TRIGGERING_ACTOR':'palmoni5'}
    assert all(env.get(k)==v for k,v in expected.items())
    source=env['GITHUB_SHA'];assert re.fullmatch('[0-9a-f]{40}',source)
    assert source==env.get('GITHUB_WORKFLOW_SHA')==env.get('GESHER_COHERENT_TCP_AUTHORIZED_SOURCE')
    assert env.get('GESHER_COHERENT_TCP_POLICY_REF')==REPOSITORY+'/'+PATH+'@'+source
    assert all(re.fullmatch('[1-9][0-9]*',env[k]) for k in ('GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'))
    if job in ('compile','windows','supplier-correlation'):assert env.get('TCP_SUPPLIER_ROLE')=='selected-minimum'
    return {k:env[k] for k in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY','GITHUB_REPOSITORY_ID')}

if __name__=='__main__':
    import json,os
    print('COHERENT_TCP_ROLE '+json.dumps(context(os.environ,os.environ['GITHUB_JOB']),sort_keys=True))
