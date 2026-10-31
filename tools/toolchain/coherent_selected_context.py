"""Exact selected supplier caller for a future independently guarded coherent full.

Source-only WIP: this module supplies no authority until an immutable guard
actually emits the exact source and policy values and the full policy is peered.
"""
import re

REPOSITORY='gesher-runtime/gesher'
REPOSITORY_ID='1426153186'
BRANCH='wip/coherent-selected-tcp'
CALLER='coherent-selected-full/build'
WORKFLOW='ci'
PATH='.github/workflows/ci.yml'
POLICY_PATH='.github/workflows/net-combined-dns-trusted-source.yml'
TUPLE=dict(repository=REPOSITORY,repository_id=REPOSITORY_ID,ref='refs/heads/'+BRANCH,
           workflow=WORKFLOW,workflow_path=PATH,job='build',caller=CALLER,
           workflow_ref=REPOSITORY+'/'+PATH+'@refs/heads/'+BRANCH,
           source_authority='dedicated immutable full guard and caller key')

def context(env,selection):
    assert selection=='selected-minimum' and env.get('GESHER_HOST_PTHREAD_SELECTION')==selection
    assert env.get('GITHUB_ACTIONS')=='true' and env.get('GITHUB_EVENT_NAME')=='workflow_dispatch'
    assert env.get('GITHUB_ACTOR')==env.get('GITHUB_TRIGGERING_ACTOR')=='palmoni5'
    for key,want in (('GITHUB_REPOSITORY',REPOSITORY),('GITHUB_REPOSITORY_ID',REPOSITORY_ID),
                     ('GITHUB_REF',TUPLE['ref']),('GITHUB_WORKFLOW',WORKFLOW),
                     ('GITHUB_WORKFLOW_REF',TUPLE['workflow_ref']),('GITHUB_JOB','build'),
                     ('GESHER_HOST_PTHREAD_CALLER',CALLER)):
        assert env.get(key)==want,('Wrong coherent caller field',key)
    source=env['GITHUB_SHA']
    assert re.fullmatch('[0-9a-f]{40}',source)
    assert source==env.get('GITHUB_WORKFLOW_SHA')==env.get('GESHER_COHERENT_FULL_AUTHORIZED_SOURCE')
    for key in ('GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'): assert re.fullmatch('[1-9][0-9]*',env[key])
    assert re.fullmatch(re.escape(REPOSITORY+'/'+POLICY_PATH+'@')+'[0-9a-f]{40}',env['GESHER_COHERENT_FULL_POLICY_REF'])
    return {k:env[k] for k in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY','GITHUB_REPOSITORY_ID')}

def descriptor(env):
    context(env,'selected-minimum')
    return dict(TUPLE,source=env['GITHUB_SHA'],policy_ref=env['GESHER_COHERENT_FULL_POLICY_REF'])

if __name__=='__main__':
    import json,os
    print('COHERENT_SELECTED_CALLER '+json.dumps(descriptor(os.environ),sort_keys=True))
