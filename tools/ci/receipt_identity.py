"""Exact workflow-run identity predicate and adversarial CI checks."""
def exact_run(run, wanted_id, wanted_attempt, wanted_head, wanted_repo, wanted_head_repo, wanted_name, wanted_workflow='ci'):
    return (str(run['id']) == wanted_id and str(run['run_attempt']) == wanted_attempt and run['head_sha'] == wanted_head
            and str(run['repository']['id']) == wanted_repo and str(run['head_repository']['id']) == wanted_head_repo
            and run['repository']['full_name'] == wanted_name and run['name'] == wanted_workflow)

def failed_installer_allowed(workflow, job, producer, artifact, suffix):
    return workflow == 'ci' and job == 'e2e' and producer == 'package' and artifact == 'gesher-setup' and not suffix

def installer_uploaded(job):
    steps = [step for step in job['steps'] if step['name'] == 'Upload tested installer artifact']
    return len(steps) == 1 and steps[0]['conclusion'] == 'success'

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
    valid = ('ci', 'e2e', 'package', 'gesher-setup', False)
    if not failed_installer_allowed(*valid):
        raise RuntimeError('Valid e2e policy rejected')
    for values in [('other','e2e','package','gesher-setup',False), ('ci','test','package','gesher-setup',False), ('ci','e2e','build','gesher-setup',False), ('ci','e2e','package','framework',False), ('ci','e2e','package','gesher-setup',True)]:
        if failed_installer_allowed(*values):
            raise RuntimeError('Unrelated failed-producer exception accepted')
    if not installer_uploaded({'steps':[{'name':'Upload tested installer artifact','conclusion':'success'}]}) or installer_uploaded({'steps':[{'name':'Upload tested installer artifact','conclusion':'failure'}]}) or installer_uploaded({'steps':[]}):
        raise RuntimeError('Installer upload policy control failed')
    print('CI_RECEIPT_INSTALLER_NEGATIVES wrong-context/failed-or-missing-upload=rejected', flush=True)
