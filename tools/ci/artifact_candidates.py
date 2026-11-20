"""Choose the unique exact-run artifact uploaded during its proved producer job."""
from datetime import datetime
import json
import os
import re
import sys
import time
from urllib.parse import urlencode

from github_jobs import get_jobs
from github_read import GitHubReadError, get_json


_DIGEST = re.compile(r'sha256:[0-9a-f]{64}')


class ArtifactInventoryChanged(RuntimeError):
    pass


def _inventory(endpoint, name, read):
    # A per-name receipt need not read the whole run. A batch of named roles
    # reads a single bounded snapshot to avoid one API call per role.
    pages = 1 if name is not None else 5
    rows = []
    ids = set()
    count = None
    for page in range(1, pages + 1):
        parameters = {'per_page': 100, 'page': page}
        if name is not None:
            parameters['name'] = name
        value = read(f'{endpoint}?{urlencode(parameters)}')
        total = value.get('total_count') if isinstance(value, dict) else None
        items = value.get('artifacts') if isinstance(value, dict) else None
        if (type(total) is not int or not 0 <= total <= 100 * pages
                or not isinstance(items, list) or len(items) > 100):
            raise RuntimeError('Invalid or incomplete artifact inventory')
        if count is not None and total != count:
            raise ArtifactInventoryChanged('Artifact inventory count changed')
        count = total
        if len(items) != min(100, count - len(rows)):
            raise ArtifactInventoryChanged('Artifact inventory page is incomplete')
        for item in items:
            identity = item.get('id') if isinstance(item, dict) else None
            if (type(identity) is not int or identity <= 0 or identity in ids
                    or (name is not None and item.get('name') != name)):
                raise RuntimeError('Invalid, repeated or unexpected artifact identity')
            ids.add(identity)
        rows.extend(items)
        if len(rows) == count:
            return rows
    raise RuntimeError('Incomplete artifact inventory')


def read_artifacts(endpoint, *, name=None, read=get_json):
    for attempt in range(3):
        try:
            return _inventory(endpoint, name, read)
        except ArtifactInventoryChanged:
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def _timestamp(value):
    try:
        result = datetime.fromisoformat(value.replace('Z', '+00:00'))
    except (AttributeError, TypeError, ValueError):
        raise RuntimeError('Missing or invalid artifact/producer timestamp') from None
    if result.tzinfo is None:
        raise RuntimeError('Artifact/producer timestamp lacks an offset')
    return result


def select_artifact(rows, name, job, *, run_id, repo_id, head_repo_id, head):
    start, end = _timestamp(job.get('started_at')), _timestamp(job.get('completed_at'))
    if start > end:
        raise RuntimeError('Producer time window is inverted')
    matching = []
    for item in rows:
        if item['name'] != name:
            continue
        source = item.get('workflow_run')
        if (not isinstance(source, dict) or str(source.get('id')) != run_id
                or source.get('head_sha') != head or str(source.get('repository_id')) != repo_id
                or str(source.get('head_repository_id')) != head_repo_id):
            raise RuntimeError('Artifact identity mismatch')
        created = _timestamp(item.get('created_at'))
        if created < start:
            continue
        if created > end:
            raise RuntimeError('Same-name artifact appeared after the exact producer completed')
        matching.append(item)
    if len(matching) != 1:
        raise RuntimeError('Artifact missing or ambiguous in exact producer window')
    item = matching[0]
    digest = item.get('digest')
    size = item.get('size_in_bytes')
    if (item.get('expired') is not False or type(size) is not int or size <= 0
            or not isinstance(digest, str) or not _DIGEST.fullmatch(digest)):
        raise RuntimeError('Artifact digest, size or expiry mismatch')
    return item


def main():
    # The PowerShell caller first proves its canonical run, then identifies
    # the roles this workflow requires. No fallback supplier is allowed.
    if os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('CI only')
    repo = os.environ['GITHUB_REPOSITORY']
    run_id, attempt = os.environ['GITHUB_RUN_ID'], os.environ['GITHUB_RUN_ATTEMPT']
    repo_id = os.environ['GITHUB_REPOSITORY_ID']
    request = json.load(sys.stdin)
    head, head_repo_id = request['head'], request['head_repository_id']
    roles, extra = request['roles'], request['additional_jobs']
    if (not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo)
            or not all(re.fullmatch(r'[1-9][0-9]*', value) for value in (run_id, attempt, repo_id, head_repo_id))
            or not re.fullmatch(r'[0-9a-f]{40}', head) or not isinstance(roles, dict) or not roles
            or not isinstance(extra, list) or not all(isinstance(name, str) and name for name in [*roles.values(), *extra])):
        raise RuntimeError('Invalid batch artifact identity')
    endpoint = f'repos/{repo}/actions/runs/{run_id}'
    jobs = get_jobs(f'{endpoint}/attempts/{attempt}/jobs')['jobs']
    proven = {}
    for name in dict.fromkeys([*roles.values(), *extra]):
        rows = [job for job in jobs if job['name'] == name]
        if len(rows) != 1:
            raise RuntimeError(f'Full supplier job missing or ambiguous: {name}')
        job = rows[0]
        if (type(job['id']) is not int or job['id'] <= 0
                or job['status'] != 'completed' or job['conclusion'] != 'success'
                or job['head_sha'] != head or str(job['run_id']) != run_id or str(job['run_attempt']) != attempt):
            raise RuntimeError(f'Full supplier is not a successful exact-attempt job: {name}')
        proven[name] = job
    artifacts = read_artifacts(f'{endpoint}/artifacts')
    result = {
        name: select_artifact(artifacts, name, proven[producer], run_id=run_id, repo_id=repo_id,
                              head_repo_id=head_repo_id, head=head)
        for name, producer in roles.items()
    }
    print(json.dumps(result, ensure_ascii=True))


if __name__ == '__main__':
    try:
        main()
    except (GitHubReadError, RuntimeError, KeyError, TypeError, ValueError) as error:
        print(f'CI_ARTIFACT_SELECTION_ERROR {error}', file=sys.stderr)
        sys.exit(1)
