"""Exact selected-only current-source TCP roles within coherent full CI."""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'ci'))
from source_identity import source_identity

REPOSITORY='gesher-runtime/gesher'; REPOSITORY_ID='1426153186'
BRANCH='wip/coherent-selected-tcp'; WORKFLOW='ci'
PATH='.github/workflows/ci.yml'

def context(env,job,event=None,checkout_parents=None):
    assert job in ('compile','linux-reference','windows','supplier-correlation') and env.get('GITHUB_JOB')==job
    identity = source_identity(env, event, checkout_parents)
    ref = env['GITHUB_REF']
    if env['GITHUB_EVENT_NAME'] == 'pull_request':
        assert env['GITHUB_BASE_REF'] == 'main'
    else:
        assert ref in ('refs/heads/'+BRANCH, 'refs/heads/main')
    assert env.get('GITHUB_WORKFLOW_REF') == REPOSITORY+'/'+PATH+'@'+ref
    expected={'GITHUB_ACTIONS':'true','GITHUB_REPOSITORY':REPOSITORY,'GITHUB_REPOSITORY_ID':REPOSITORY_ID,
              'GITHUB_WORKFLOW':WORKFLOW,
              'GITHUB_ACTOR':'palmoni5','GITHUB_TRIGGERING_ACTOR':'palmoni5'}
    assert all(env.get(k)==v for k,v in expected.items())
    source=identity['checkout_sha']
    assert source==env.get('GITHUB_WORKFLOW_SHA')==env.get('GESHER_COHERENT_TCP_AUTHORIZED_SOURCE')
    assert env.get('GESHER_COHERENT_TCP_POLICY_REF')==REPOSITORY+'/'+PATH+'@'+source
    assert all(re.fullmatch('[1-9][0-9]*',env[k]) for k in ('GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'))
    if job in ('compile','windows','supplier-correlation'):assert env.get('TCP_SUPPLIER_ROLE')=='selected-minimum'
    return {k:env[k] for k in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY','GITHUB_REPOSITORY_ID')}

if __name__=='__main__':
    import json,os
    print('COHERENT_TCP_ROLE '+json.dumps(context(os.environ,os.environ['GITHUB_JOB']),sort_keys=True))
