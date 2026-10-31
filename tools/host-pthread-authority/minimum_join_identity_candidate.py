"""Source-only identity predicates for a future isolated diagnostic lane.

These pure functions grant no authority. A separate immutable Git-object gate,
live API transport, producer receipts and current binary proof remain required.
"""
import re

REPOSITORY = 'gesher-runtime/gesher'
REPOSITORY_ID = 1426153186
BRANCH = 'wip/pthread-minimum-join-cancellation'
WORKFLOW = 'pthread-minimum-join-cancel-component'
CALLER = '.github/workflows/pthread-minimum-join-cancel-component.yml'
GUARD = '.github/workflows/pthread-minimum-join-cancel-trusted-source.yml'
VARIABLE = 'PTHREAD_MINIMUM_JOIN_CANCEL_CALLER_COMMIT'
JOBS = frozenset(('verify', 'diagnostic-producer', 'diagnostic-windows'))


def verify_context(env, job):
    if job not in JOBS:
        raise ValueError('unsupported role')
    expected = {'GITHUB_ACTIONS': 'true', 'GITHUB_REPOSITORY': REPOSITORY,
                'GITHUB_REPOSITORY_ID': str(REPOSITORY_ID),
                'GITHUB_REF': 'refs/heads/' + BRANCH, 'GITHUB_WORKFLOW': WORKFLOW,
                'GITHUB_WORKFLOW_REF': REPOSITORY + '/' + CALLER + '@refs/heads/' + BRANCH,
                'GITHUB_JOB': job, 'GITHUB_ACTOR': 'palmoni5',
                'GITHUB_TRIGGERING_ACTOR': 'palmoni5'}
    if any(env.get(key) != value for key, value in expected.items()):
        raise ValueError('caller identity')
    sha = env.get('GITHUB_SHA', '')
    if (not re.fullmatch('[0-9a-f]{40}', sha) or env.get('GITHUB_WORKFLOW_SHA') != sha
            or env.get(VARIABLE) != sha or env.get('GITHUB_EVENT_NAME') not in ('push', 'workflow_dispatch')):
        raise ValueError('source or event')
    if any(not re.fullmatch('[1-9][0-9]*', env.get(key, '')) for key in ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT')):
        raise ValueError('run identity')
    return {'source': sha, 'run': int(env['GITHUB_RUN_ID']), 'attempt': int(env['GITHUB_RUN_ATTEMPT'])}


def verify_run(run, context, guard):
    if not isinstance(guard, str) or not re.fullmatch('[0-9a-f]{40}', guard):
        raise ValueError('guard SHA')
    expected = {'id': context['run'], 'run_attempt': context['attempt'], 'head_sha': context['source'],
                'head_branch': BRANCH, 'path': CALLER, 'name': WORKFLOW}
    if any(run.get(key) != value for key, value in expected.items()):
        raise ValueError('live run')
    if run.get('event') not in ('push', 'workflow_dispatch'):
        raise ValueError('event')
    for key in ('repository', 'head_repository'):
        value = run.get(key)
        if type(value) is not dict or value.get('id') != REPOSITORY_ID or value.get('full_name') != REPOSITORY:
            raise ValueError('repository')
    for key in ('actor', 'triggering_actor'):
        value = run.get(key)
        if type(value) is not dict or value.get('login') != 'palmoni5':
            raise ValueError('actor')
    refs = run.get('referenced_workflows')
    if (type(refs) is not list or len(refs) != 1 or type(refs[0]) is not dict
            or refs[0].get('path') != REPOSITORY + '/' + GUARD + '@' + guard or refs[0].get('sha') != guard):
        raise ValueError('referenced immutable guard')


def verify_producer(jobs, context):
    if type(jobs) is not list or len(jobs) > 100:
        raise ValueError('job inventory')
    found = [row for row in jobs if row.get('name') == 'diagnostic-producer']
    if len(found) != 1:
        raise ValueError('producer uniqueness')
    row = found[0]
    if (row.get('head_sha') != context['source'] or row.get('run_id') != context['run']
            or row.get('run_attempt') != context['attempt'] or row.get('status') != 'completed'
            or row.get('conclusion') != 'success' or type(row.get('id')) is not int or row['id'] < 1):
        raise ValueError('producer identity')
    return row['id']
