"""Complete, bounded job inventories: one 100-row page, or two 50-row pages."""
import argparse
import json
import sys
import time

from github_read import GitHubReadError, GitHubTransientError, get_json


class _InventoryChanged(GitHubReadError):
    pass


def get_jobs(endpoint):
    """Read at most 100 jobs without accepting a partial or changing inventory.

    One 100-row request is tried first: it halves the REST calls of every read
    and so the repository's shared GITHUB_TOKEN quota. GitHub can return 502
    for a 100-row attempt inventory while both 50-row pages succeed, so an
    observed transient failure of that single request (not retried) falls back
    to the 50-row pages. Paging changes transport size, never producer acceptance.
    """
    try:
        return _consistent_snapshot(endpoint, _single_page)
    except GitHubTransientError:
        print('CI_GITHUB_JOBS_FALLBACK per_page=50', file=sys.stderr, flush=True)
    return _consistent_snapshot(endpoint, _paged_snapshot)


def _consistent_snapshot(endpoint, read):
    # Counts and rows can disagree even within one response. Discard the whole
    # snapshot; neither a partial page nor rows from an earlier read may escape.
    # Retry only this validated inconsistency, never malformed IDs or metadata.
    for attempt in range(3):
        try:
            return read(endpoint)
        except _InventoryChanged as error:
            print(f'CI_GITHUB_JOBS_INCONSISTENT attempt={attempt + 1}/3 '
                  f'retry={str(attempt < 2).lower()} {error}', file=sys.stderr, flush=True)
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def _validated(value, page_size):
    count = value.get('total_count') if isinstance(value, dict) else None
    items = value.get('jobs') if isinstance(value, dict) else None
    if (type(count) is not int or not 0 <= count <= 100
            or not isinstance(items, list) or len(items) > page_size):
        raise GitHubReadError('Invalid or oversized job inventory')
    return count, items


def _single_page(endpoint):
    value = get_json(f'{endpoint}?per_page=100&page=1', retry_transient=False)
    count, items = _validated(value, 100)
    seen = set()
    for job in items:
        job_id = job.get('id') if isinstance(job, dict) else None
        if type(job_id) is not int or job_id <= 0 or job_id in seen:
            raise GitHubReadError('Invalid or repeated job identity')
        seen.add(job_id)
    if len(items) != count:
        raise _InventoryChanged('Inconsistent job page: per_page=100 page=1 '
                                f'total_count={count} received_count={len(items)}')
    return {'total_count': count, 'jobs': items}


def _paged_snapshot(endpoint):
    total = None
    rows = []
    seen = set()
    for page in (1, 2):
        value = get_json(f'{endpoint}?per_page=50&page={page}')
        count, items = _validated(value, 50)
        for job in items:
            job_id = job.get('id') if isinstance(job, dict) else None
            if type(job_id) is not int or job_id <= 0 or job_id in seen:
                raise GitHubReadError('Invalid or repeated job identity')
            seen.add(job_id)
        if total is not None and count != total:
            raise _InventoryChanged('Job inventory changed between pages: '
                                    f'page={page} expected_total_count={total} total_count={count}')
        total = count
        rows.extend(items)
        if len(rows) != min(50 * page, total):
            raise _InventoryChanged(f'Inconsistent job page: per_page=50 page={page} '
                                    f'total_count={total} received_count={len(rows)} '
                                    f'expected_count={min(50 * page, total)}')
        if len(rows) == total:
            return {'total_count': total, 'jobs': rows}
    raise GitHubReadError('Incomplete job inventory')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('endpoint', help='exact GitHub Actions attempt jobs endpoint without a query')
    args = parser.parse_args()
    try:
        jobs = get_jobs(args.endpoint)
    except GitHubReadError as error:
        print(f'CI_GITHUB_JOBS_ERROR {error}', file=sys.stderr)
        return 1
    print(json.dumps(jobs, ensure_ascii=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
