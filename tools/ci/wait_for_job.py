"""Wait inside a running CI job until one exact current-run job succeeds.

A job that consumes another job's artifact only at its end can start without
a `needs:` edge and wait here just before the download, so its own long work
overlaps the producer instead of following it. The wait never accepts the
artifact by itself: the consumer still downloads it with artifact_receipt.py,
which requires the same producer to have completed successfully.
"""
import argparse
import os
import sys
import time

from github_jobs import get_jobs
from github_read import GitHubReadError

# Consecutive failed inventory reads tolerated before giving up.
READ_FAILURES = 5
# Consecutive inventories without the job before it is treated as absent.
MISSING_READS = 3


def current_endpoint(environ=os.environ):
    if environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('CI only')
    return (f"repos/{environ['GITHUB_REPOSITORY']}/actions/runs/{environ['GITHUB_RUN_ID']}"
            f"/attempts/{environ['GITHUB_RUN_ATTEMPT']}/jobs")


class Missing(RuntimeError):
    """The job is not in this run attempt's inventory."""


def state(jobs, name):
    """None while the job is not finished; the job once it has succeeded."""
    rows = [row for row in jobs['jobs'] if row.get('name') == name]
    if len(rows) > 1:
        raise RuntimeError(f'Ambiguous job name: {name}')
    if not rows:
        raise Missing(f'Job {name} is not in this run attempt')
    if rows[0].get('status') != 'completed':
        return None
    if rows[0].get('conclusion') != 'success':
        raise RuntimeError(f"Job {name} finished with {rows[0].get('conclusion')}")
    return rows[0]


def wait(read, name, *, timeout, interval, sleep=time.sleep, clock=time.monotonic, log=print):
    deadline = clock() + timeout
    failures = 0
    missing = 0
    while True:
        try:
            job = state(read(), name)
            failures = missing = 0
        except Missing:
            # Jobs of one attempt are listed from its start. A producer that
            # stays absent (for example a partial rerun) never arrives here.
            missing += 1
            if missing >= MISSING_READS:
                raise
            job = None
        except GitHubReadError as error:
            failures += 1
            if failures >= READ_FAILURES:
                raise
            log(f'WAIT_FOR_JOB read failed ({failures}/{READ_FAILURES}): {error}', flush=True)
            job = None
        if job is not None:
            log(f"WAIT_FOR_JOB name={name} id={job.get('id')} conclusion=success", flush=True)
            return job
        if clock() >= deadline:
            raise TimeoutError(f'Job {name} did not finish within {timeout}s')
        sleep(interval)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--job', required=True, help='exact job name in the current run attempt')
    parser.add_argument('--timeout', type=int, default=3600, help='seconds')
    parser.add_argument('--interval', type=int, default=20, help='seconds between reads')
    args = parser.parse_args(argv)
    endpoint = current_endpoint()
    wait(lambda: get_jobs(endpoint), args.job, timeout=args.timeout, interval=args.interval)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, TimeoutError, GitHubReadError) as error:
        print(f'WAIT_FOR_JOB failed: {error}', file=sys.stderr, flush=True)
        sys.exit(1)
