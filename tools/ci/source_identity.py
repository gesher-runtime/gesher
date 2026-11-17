"""Distinguish a PR merge checkout from GitHub's API run/head identity."""
import json
import os
from pathlib import Path
import re
import subprocess


def source_identity(env, event=None, checkout_parents=None):
    checkout = env['GITHUB_SHA']
    assert re.fullmatch(r'[0-9a-f]{40}', checkout), 'Invalid checkout SHA'
    kind = env['GITHUB_EVENT_NAME']
    assert kind in ('push', 'workflow_dispatch', 'pull_request', 'merge_group'), 'Unsupported source event'
    if kind == 'merge_group':
        if event is None:
            event = json.loads(Path(env['GITHUB_EVENT_PATH']).read_text(encoding='utf-8-sig'))
        group = event['merge_group']
        assert event['action'] == 'checks_requested', 'Unexpected merge group action'
        assert group['head_sha'] == checkout, 'Merge group checkout mismatch'
        assert group['head_ref'] == env['GITHUB_REF'], 'Merge group ref mismatch'
        assert group['head_ref'].startswith('refs/heads/gh-readonly-queue/main/'), 'Unexpected queue ref'
        assert group['base_ref'] == 'refs/heads/main', 'Unexpected merge group target'
        assert re.fullmatch(r'[0-9a-f]{40}', group['base_sha']), 'Invalid merge group base'
        return {'checkout_sha': checkout, 'api_head_sha': checkout,
                'head_repository_id': env['GITHUB_REPOSITORY_ID']}
    if kind != 'pull_request':
        return {'checkout_sha': checkout, 'api_head_sha': checkout,
                'head_repository_id': env['GITHUB_REPOSITORY_ID']}

    if event is None:
        event = json.loads(Path(env['GITHUB_EVENT_PATH']).read_text(encoding='utf-8-sig'))
    number = event['number']
    assert type(number) is int and number > 0, 'Invalid PR number'
    assert env['GITHUB_REF'] == f'refs/pull/{number}/merge', 'Unexpected PR ref'
    pr = event['pull_request']
    repo = pr['base']['repo']
    assert str(repo['id']) == env['GITHUB_REPOSITORY_ID'], 'Foreign PR target repository'
    assert repo['full_name'] == env['GITHUB_REPOSITORY'], 'PR target repository name mismatch'
    head_repo = pr['head']['repo']
    assert type(head_repo['id']) is int and head_repo['id'] > 0, 'Invalid PR source repository ID'
    assert re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', head_repo['full_name']), 'Invalid PR source repository name'
    assert (str(head_repo['id']) == env['GITHUB_REPOSITORY_ID']) == (head_repo['full_name'] == env['GITHUB_REPOSITORY']), 'Inconsistent PR source repository identity'
    for key in ('base', 'head'):
        assert pr[key]['ref'] == env[f'GITHUB_{key.upper()}_REF'], 'PR branch mismatch'
    head = pr['head']['sha']
    assert re.fullmatch(r'[0-9a-f]{40}', head), 'Invalid PR head SHA'
    base = pr['base']['sha']
    assert re.fullmatch(r'[0-9a-f]{40}', base), 'Invalid PR base SHA'
    # synchronize payloads can still name an older merge_commit_sha or base SHA.
    # Bind the actual checkout to the PR head through its raw commit headers.
    # cat-file retains parents even when checkout used fetch-depth: 1.
    if checkout_parents is None:
        assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == checkout, 'Checkout SHA mismatch'
        headers = subprocess.check_output(['git', 'cat-file', '-p', checkout], text=True).split('\n\n', 1)[0]
        checkout_parents = tuple(line[7:] for line in headers.splitlines() if line.startswith('parent '))
    parents = tuple(checkout_parents)
    detail = f'checkout={checkout}, payload_base={base}, head={head}, parents={parents}'
    assert len(parents) == 2 and parents[1] == head, f'PR checkout parent mismatch: {detail}'
    if parents[0] != base:
        # GitHub may create the merge against a newer target tip while keeping
        # the PR payload's original base. Accept only the requested target's
        # fetched history: payload base -> tested base -> fetched target tip.
        # A target that advanced before checkout remains valid; unrelated or
        # rewritten ancestry, missing history and missing refs fail closed.
        target_ref = f"refs/remotes/origin/{env['GITHUB_BASE_REF']}"
        try:
            target = subprocess.check_output(
                ['git', 'show-ref', '--verify', '--hash', target_ref],
                text=True, stderr=subprocess.PIPE).strip()
        except subprocess.CalledProcessError as error:
            raise AssertionError(f'Cannot prove PR target history ({target_ref}); '
                                 f'checkout requires fetch-depth: 0: {detail}') from error
        assert re.fullmatch(r'[0-9a-f]{40}', parents[0]), f'Invalid tested PR base: {detail}'
        for older, newer in ((base, parents[0]), (parents[0], target)):
            proof = subprocess.run(['git', 'merge-base', '--is-ancestor', older, newer],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            assert proof.returncode == 0, (f'Unproved PR target ancestry {older} -> {newer}, '
                                          f'target={target_ref}@{target}, git_exit={proof.returncode}: {detail}')
    # A fork is a distinct source repository, not a different target. Consumers
    # must bind artifact metadata to this event-supplied ID and the PR head SHA.
    return {'checkout_sha': checkout, 'api_head_sha': head,
            'head_repository_id': str(head_repo['id'])}


if __name__ == '__main__':
    assert os.environ.get('GITHUB_ACTIONS') == 'true', 'CI only'
    print(json.dumps(source_identity(os.environ), sort_keys=True))
