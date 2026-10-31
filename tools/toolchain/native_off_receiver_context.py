"""One explicit focused caller, never a display-name or wildcard permission."""
import argparse
import json
import re

REPOSITORY = 'gesher-runtime/gesher'
REPOSITORY_ID = '1426153186'
CALLER = 'pthread-native-off-receiver/build'
BRANCH = 'wip/pthread-native-runtime161'
WORKFLOW = 'pthread-native-off-receiver-component'
PATH = '.github/workflows/pthread-native-off-receiver-component.yml'
POLICY_PATH = '.github/workflows/pthread-native-off-receiver-trusted-source.yml'
TUPLE = dict(repository=REPOSITORY, repository_id=REPOSITORY_ID, ref='refs/heads/'+BRANCH,
             workflow=WORKFLOW, workflow_path=PATH, job='build', caller=CALLER,
             workflow_ref=REPOSITORY+'/'+PATH+'@refs/heads/'+BRANCH,
             source_authority='dedicated immutable reusable policy + PTHREAD_NATIVE_RUNTIME161_CALLER_COMMIT')


def common_context(env, selection):
    assert selection == 'selected-minimum' and env.get('GESHER_HOST_PTHREAD_SELECTION') == selection
    assert env.get('GITHUB_ACTIONS') == 'true'
    for key, expected in (('GITHUB_REPOSITORY', REPOSITORY), ('GITHUB_REPOSITORY_ID', REPOSITORY_ID),
                          ('GITHUB_REF', TUPLE['ref']), ('GITHUB_WORKFLOW', WORKFLOW),
                          ('GITHUB_WORKFLOW_REF', TUPLE['workflow_ref'])):
        assert env.get(key) == expected, ('Wrong focused caller field', key)
    source = env['GITHUB_SHA']
    assert re.fullmatch('[0-9a-f]{40}', source)
    assert source == env.get('GITHUB_WORKFLOW_SHA') == env.get('GESHER_NATIVE_OFF_RECEIVER_AUTHORIZED_SOURCE')
    assert env.get('GITHUB_EVENT_NAME') in ('push', 'workflow_dispatch')
    assert env.get('GITHUB_ACTOR') == env.get('GITHUB_TRIGGERING_ACTOR') == 'palmoni5'
    for key in ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT'):
        assert re.fullmatch('[1-9][0-9]*', env[key])
    policy = env['GESHER_NATIVE_OFF_RECEIVER_POLICY_REF']
    assert re.fullmatch(re.escape(REPOSITORY+'/'+POLICY_PATH+'@')+'[0-9a-f]{40}', policy)
    return {k: env[k] for k in ('GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT',
                               'GITHUB_REPOSITORY', 'GITHUB_REPOSITORY_ID')}


def context(env, selection):
    result=common_context(env,selection)
    assert env.get('GITHUB_JOB')=='build' and env.get('GESHER_HOST_PTHREAD_CALLER')==CALLER
    return result


def consumer_context(env, selection):
    result=common_context(env,selection)
    assert env.get('GITHUB_JOB')=='native3-locks-regression' and env.get('GESHER_HOST_PTHREAD_CALLER')=='pthread-native-off-receiver/receiver'
    return result


def reference_context(env):
    result=common_context(env,'selected-minimum')
    assert env.get('GITHUB_JOB')=='native3-locks-reference' and env.get('GESHER_HOST_PTHREAD_CALLER')=='pthread-native-off-receiver/reference'
    return result


def descriptor(env):
    context(env, 'selected-minimum')
    return dict(TUPLE, source=env['GITHUB_SHA'], policy_ref=env['GESHER_NATIVE_OFF_RECEIVER_POLICY_REF'])


def check_descriptor(value, expected):
    assert set(value) == set(TUPLE) | {'source', 'policy_ref'}
    assert all(value[k] == v for k, v in TUPLE.items())
    assert value['source'] == expected['GITHUB_SHA']
    assert expected['GITHUB_REPOSITORY'] == REPOSITORY and expected['GITHUB_REPOSITORY_ID'] == REPOSITORY_ID
    assert re.fullmatch(re.escape(REPOSITORY+'/'+POLICY_PATH+'@')+'[0-9a-f]{40}', value['policy_ref'])


if __name__ == '__main__':
    import os
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', action='store_true', required=True)
    parser.parse_args()
    context(os.environ, 'selected-minimum')
    print('NATIVE_OFF_RECEIVER_EXPLICIT_CALLER '+json.dumps(descriptor(os.environ), sort_keys=True))
