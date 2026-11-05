"""Distinguish a PR merge checkout from GitHub's API run/head identity."""
import json
import os
from pathlib import Path
import re


def source_identity(env, event=None):
    checkout = env['GITHUB_SHA']
    assert re.fullmatch(r'[0-9a-f]{40}', checkout), 'Invalid checkout SHA'
    kind = env['GITHUB_EVENT_NAME']
    assert kind in ('push', 'workflow_dispatch', 'pull_request'), 'Unsupported source event'
    if kind != 'pull_request':
        return {'checkout_sha': checkout, 'api_head_sha': checkout}

    if event is None:
        event = json.loads(Path(env['GITHUB_EVENT_PATH']).read_text(encoding='utf-8-sig'))
    number = event['number']
    assert type(number) is int and number > 0, 'Invalid PR number'
    assert env['GITHUB_REF'] == f'refs/pull/{number}/merge', 'Unexpected PR ref'
    pr = event['pull_request']
    assert pr['merge_commit_sha'] == checkout, 'PR merge checkout mismatch'
    for key in ('base', 'head'):
        repo = pr[key]['repo']
        assert str(repo['id']) == env['GITHUB_REPOSITORY_ID'], 'Foreign PR repository'
        assert repo['full_name'] == env['GITHUB_REPOSITORY'], 'PR repository name mismatch'
        assert pr[key]['ref'] == env[f'GITHUB_{key.upper()}_REF'], 'PR branch mismatch'
    head = pr['head']['sha']
    assert re.fullmatch(r'[0-9a-f]{40}', head), 'Invalid PR head SHA'
    return {'checkout_sha': checkout, 'api_head_sha': head}


if __name__ == '__main__':
    assert os.environ.get('GITHUB_ACTIONS') == 'true', 'CI only'
    print(json.dumps(source_identity(os.environ), sort_keys=True))
