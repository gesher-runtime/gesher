"""Pure exact-authority controls; no source approval or CI execution inferred."""
import copy
import first_link_context as focused
import prepare_host_pthread as factory


def main():
    env=dict(GITHUB_ACTIONS='true',GITHUB_REPOSITORY=focused.REPOSITORY,GITHUB_REPOSITORY_ID=focused.REPOSITORY_ID,
             GITHUB_REF=focused.TUPLE['ref'],GITHUB_WORKFLOW=focused.WORKFLOW,GITHUB_WORKFLOW_REF=focused.TUPLE['workflow_ref'],
             GITHUB_JOB='build',GESHER_HOST_PTHREAD_CALLER=focused.CALLER,GESHER_HOST_PTHREAD_SELECTION='selected-minimum',
             GITHUB_SHA='1'*40,GITHUB_WORKFLOW_SHA='1'*40,GESHER_FIRST_LINK_AUTHORIZED_SOURCE='1'*40,
             GITHUB_EVENT_NAME='push',GITHUB_ACTOR='palmoni5',GITHUB_TRIGGERING_ACTOR='palmoni5',
             GITHUB_RUN_ID='123456',GITHUB_RUN_ATTEMPT='1',
             GESHER_FIRST_LINK_POLICY_REF=focused.REPOSITORY+'/'+focused.POLICY_PATH+'@'+'2'*40)
    context=focused.context(env,'selected-minimum')
    assert factory.caller(env,'selected-minimum')==context
    focused.check_descriptor(focused.descriptor(env),context)
    bad_values=dict(GITHUB_ACTIONS='false',GITHUB_REPOSITORY='other/gesher',GITHUB_REPOSITORY_ID='1',
                    GITHUB_REF='refs/heads/main',GITHUB_WORKFLOW='ci',GITHUB_WORKFLOW_REF=focused.REPOSITORY+'/.github/workflows/ci.yml@'+focused.TUPLE['ref'],
                    GITHUB_JOB='compile',GESHER_HOST_PTHREAD_CALLER='ci/build',GESHER_HOST_PTHREAD_SELECTION='publisher-control',
                    GITHUB_SHA='3'*40,GITHUB_WORKFLOW_SHA='3'*40,GESHER_FIRST_LINK_AUTHORIZED_SOURCE='3'*40,
                    GITHUB_EVENT_NAME='pull_request',GITHUB_ACTOR='other',GITHUB_TRIGGERING_ACTOR='other',
                    GITHUB_RUN_ID='0',GITHUB_RUN_ATTEMPT='0',GESHER_FIRST_LINK_POLICY_REF='unbound-policy')
    for key,value in bad_values.items():
        bad=dict(env);bad[key]=value
        try:focused.context(bad,'selected-minimum')
        except AssertionError:pass
        else:raise AssertionError('Wrong focused authority accepted: '+key)
    for key in ('workflow_path','workflow_ref','ref','job','caller','source','policy_ref'):
        value=copy.deepcopy(focused.descriptor(env));value[key]='unbound'
        try:focused.check_descriptor(value,context)
        except AssertionError:pass
        else:raise AssertionError('Wrong archived focused authority accepted: '+key)
    assert factory.recipe()['caller']=='ci/build'
    assert factory.recipe(focused.CALLER)['focused_caller']==focused.TUPLE
    assert factory.fingerprint(factory.recipe())!=factory.fingerprint(factory.recipe(focused.CALLER))
    print('PASS focused exact path/ref/name/job/source/policy/actor authority:18 env+7descriptor negatives; explicit recipe distinction; native/network0')


if __name__=='__main__':main()
