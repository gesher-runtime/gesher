"""Delete superseded per-commit and per-run GitHub Actions cache entries.

ccache saves a new entry per commit (ccache-<name>-<sha>) or per run
(ccache-<name>-<run_id>), and CodeQL saves an overlay base database per
commit and run. Jobs restore only the newest entry of a family through a
prefix restore key, yet every older entry stays until the repository's
cache quota evicts the least recently used ones, which pushed out the
AOSP, NDK and framework caches that misses are expensive for.

For each ref this keeps the newest entry of every such family and deletes
the rest. Entries outside those families are never touched, nor are
entries used in the last few minutes, which a running job may still be
downloading.
"""
import argparse
from datetime import datetime, timedelta, timezone
import json
import re
import subprocess
import sys

CCACHE = re.compile(r'^(ccache-.+-)(?:[0-9a-f]{40}|[0-9]+)$')
CODEQL = re.compile(r'^(codeql-overlay-base-database-.+-)[0-9a-f]{40}-[0-9]+-[0-9]+$')
GRACE = timedelta(minutes=15)


def family(key):
    """The restore prefix shared by superseded entries, or None if not governed."""
    for pattern in (CCACHE, CODEQL):
        match = pattern.match(key)
        if match:
            return match.group(1)
    return None


def when(text):
    return datetime.fromisoformat(text.replace('Z', '+00:00'))


def superseded(caches, now):
    """Entries that a newer entry of the same ref and family replaces."""
    newest = {}
    for cache in caches:
        prefix = family(cache['key'])
        if prefix is None:
            continue
        group = (cache['ref'], prefix)
        best = newest.get(group)
        if best is None or (when(cache['created_at']), cache['id']) > (when(best['created_at']), best['id']):
            newest[group] = cache
    keep = {cache['id'] for cache in newest.values()}
    return sorted(
        (cache for cache in caches
         if family(cache['key']) is not None and cache['id'] not in keep
         and now - when(cache['last_accessed_at']) >= GRACE),
        key=lambda cache: cache['id'])


def gh(*args):
    return subprocess.run(['gh', 'api', *args], check=True, capture_output=True, text=True).stdout


def list_caches(repo):
    pages = gh('--paginate', '--slurp', f'repos/{repo}/actions/caches?per_page=100')
    return [cache for page in json.loads(pages) for cache in page['actions_caches']]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--repo', required=True)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args(argv)
    caches = list_caches(args.repo)
    doomed = superseded(caches, datetime.now(timezone.utc))
    freed = 0
    for cache in doomed:
        print(f"{'would delete' if args.dry_run else 'delete'} {cache['ref']} {cache['key']} "
              f"({cache['size_in_bytes'] // 1048576} MB)")
        if not args.dry_run:
            try:
                gh('--method', 'DELETE', f"repos/{args.repo}/actions/caches/{cache['id']}")
            except subprocess.CalledProcessError as error:
                # Another prune or GitHub's own eviction may have removed it first.
                if 'HTTP 404' not in (error.stderr or ''):
                    raise
        freed += cache['size_in_bytes']
    total = sum(cache['size_in_bytes'] for cache in caches)
    print(f'{len(doomed)} of {len(caches)} entries, {freed / 2**30:.2f} of {total / 2**30:.2f} GB')
    return 0


if __name__ == '__main__':
    sys.exit(main())
