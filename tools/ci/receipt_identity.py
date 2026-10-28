"""Exact workflow-run identity predicate and adversarial CI checks."""
def exact_run(run, wanted_id, wanted_attempt, wanted_head, wanted_repo, wanted_head_repo, wanted_name, wanted_workflow='ci'):
    return (str(run['id']) == wanted_id and str(run['run_attempt']) == wanted_attempt and run['head_sha'] == wanted_head
            and str(run['repository']['id']) == wanted_repo and str(run['head_repository']['id']) == wanted_head_repo
            and run['repository']['full_name'] == wanted_name and run['name'] == wanted_workflow)

if __name__ == '__main__':
    import os
    if os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('CI only')
    sample = {'id': 7, 'run_attempt': 2, 'head_sha': 'correct', 'repository': {'id': 8, 'full_name': 'owner/repo'}, 'head_repository': {'id': 8}, 'name': 'ci'}
    if not exact_run(sample, '7', '2', 'correct', '8', '8', 'owner/repo'):
        raise RuntimeError('Valid identity rejected')
    for values in [('9','2','correct','8','8','owner/repo'), ('7','3','correct','8','8','owner/repo'), ('7','2','wrong','8','8','owner/repo'), ('7','2','correct','9','8','owner/repo')]:
        if exact_run(sample, *values):
            raise RuntimeError('Receipt identity negative accepted')
    print('CI_RECEIPT_IDENTITY_NEGATIVES run/attempt/source/repository=rejected', flush=True)
