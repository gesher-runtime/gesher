"""Authorize an internal PR's current merge commit before scheduling a runner.

Run only from the default branch on a GitHub-hosted runner. PR files, artifacts,
caches and workflow outputs are never inputs to this authorization step.
"""
import os
import re
from pathlib import Path

from github_read import get_json

REPO = 'gesher-runtime/gesher'
WORKFLOW = '.github/workflows/arm64-windows-self-hosted.yml'
SHA = re.compile(r'[0-9a-f]{40}')


def select_source(run, pull, permission, commit):
    """Return an immutable merge SHA, or reject metadata without running code."""
    if (run['event'] != 'pull_request'
            or run['repository']['full_name'] != REPO
            or run['path'] != '.github/workflows/arm64-windows.yml'
            or pull['state'] != 'open'
            or pull['base']['repo']['full_name'] != REPO
            or pull['base']['ref'] != 'main'
            or not pull['head']['repo']
            or pull['head']['repo']['full_name'] != REPO
            or pull['head']['sha'] != run['head_sha']
            or permission not in ('admin', 'maintain', 'write')):
        raise ValueError('Only current internal PRs authored by writers are eligible')
    source = pull['merge_commit_sha']
    if (not isinstance(source, str) or not SHA.fullmatch(source)
            or commit['sha'] != source
            or [p['sha'] for p in commit['parents']] != [
                pull['base']['sha'], pull['head']['sha']]):
        raise ValueError('The PR has no verified two-parent merge commit')
    return source


def main():
    if (os.environ['GITHUB_REPOSITORY'] != REPO
            or os.environ['GITHUB_REF'] != 'refs/heads/main'
            or os.environ['GITHUB_WORKFLOW_REF'] != f'{REPO}/{WORKFLOW}@refs/heads/main'):
        raise ValueError('The authorization workflow must run from main')
    run_id = os.environ['SOURCE_RUN_ID']
    if not re.fullmatch(r'[1-9][0-9]*', run_id):
        raise ValueError('A positive source run ID is required')
    run = get_json(f'repos/{REPO}/actions/runs/{run_id}')
    # Normal skips, including forks, never enqueue a self-hosted job.
    if run['event'] != 'pull_request':
        print('Not a PR run; nothing to schedule')
        return
    if not SHA.fullmatch(run['head_sha']):
        raise ValueError('Invalid run head SHA')
    candidates = get_json(f'repos/{REPO}/commits/{run["head_sha"]}/pulls?per_page=100')
    candidates = [p for p in candidates if p['state'] == 'open'
                  and p['base']['repo']['full_name'] == REPO
                  and p['base']['ref'] == 'main'
                  and p['head']['repo']
                  and p['head']['repo']['full_name'] == REPO
                  and p['head']['sha'] == run['head_sha']]
    if len(candidates) != 1:
        print('No unique current internal PR; nothing to schedule')
        return
    pull = get_json(f'repos/{REPO}/pulls/{candidates[0]["number"]}')
    permission = get_json(
        f'repos/{REPO}/collaborators/{pull["user"]["login"]}/permission')['permission']
    if permission not in ('admin', 'maintain', 'write'):
        print('PR author does not have write access; nothing to schedule')
        return
    if not pull['merge_commit_sha']:
        raise ValueError('Merge commit is not ready; retry the trial later')
    commit = get_json(f'repos/{REPO}/git/commits/{pull["merge_commit_sha"]}')
    source = select_source(run, pull, permission, commit)
    with Path(os.environ['GITHUB_OUTPUT']).open('a', encoding='utf-8') as output:
        output.write(f'allowed=true\nsource={source}\npr={int(pull["number"])}\n')
    with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a', encoding='utf-8') as summary:
        summary.write(f'Internal PR #{pull["number"]}: build merge `{source}`.\n\n'
                      f'Baseline: https://github.com/{REPO}/actions/runs/{run_id}\n')


if __name__ == '__main__':
    main()
