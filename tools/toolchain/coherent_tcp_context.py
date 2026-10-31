"""Exact selected-only current-source TCP roles within coherent full CI."""
import re
from pathlib import Path

REPOSITORY='gesher-runtime/gesher'; REPOSITORY_ID='1426153186'
BRANCH='wip/coherent-selected-tcp'; WORKFLOW='ci'
PATH='.github/workflows/ci.yml'
TRUST='.github/workflows/net-combined-dns-trusted-source.yml'

def context(env,job):
    assert job in ('compile','linux-reference','windows','supplier-correlation') and env.get('GITHUB_JOB')==job
    expected={'GITHUB_ACTIONS':'true','GITHUB_REPOSITORY':REPOSITORY,'GITHUB_REPOSITORY_ID':REPOSITORY_ID,
              'GITHUB_REF':'refs/heads/'+BRANCH,'GITHUB_WORKFLOW':WORKFLOW,
              'GITHUB_WORKFLOW_REF':REPOSITORY+'/'+PATH+'@refs/heads/'+BRANCH,
              'GITHUB_EVENT_NAME':'workflow_dispatch','GITHUB_ACTOR':'palmoni5','GITHUB_TRIGGERING_ACTOR':'palmoni5'}
    assert all(env.get(k)==v for k,v in expected.items())
    source=env['GITHUB_SHA'];assert re.fullmatch('[0-9a-f]{40}',source)
    assert source==env.get('GITHUB_WORKFLOW_SHA')==env.get('GESHER_COHERENT_TCP_AUTHORIZED_SOURCE')
    policy=env['GESHER_COHERENT_TCP_POLICY_REF']
    assert re.fullmatch(re.escape(REPOSITORY+'/'+TRUST+'@')+'[0-9a-f]{40}',policy)
    caller=(Path(__file__).resolve().parents[2]/PATH).read_text(encoding='utf-8')
    refs=re.findall(r'(?m)^    uses: '+re.escape(REPOSITORY+'/'+TRUST+'@')+r'([0-9a-f]{40})$',caller)
    assert len(refs)==1 and policy==REPOSITORY+'/'+TRUST+'@'+refs[0]
    assert all(re.fullmatch('[1-9][0-9]*',env[k]) for k in ('GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'))
    if job in ('compile','windows','supplier-correlation'):assert env.get('TCP_SUPPLIER_ROLE')=='selected-minimum'
    return {k:env[k] for k in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY','GITHUB_REPOSITORY_ID')}

if __name__=='__main__':
    import json,os
    print('COHERENT_TCP_ROLE '+json.dumps(context(os.environ,os.environ['GITHUB_JOB']),sort_keys=True))
