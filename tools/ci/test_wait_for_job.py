"""The in-job producer wait accepts only an exact, finished, successful job."""
import unittest

from github_read import GitHubReadError
from wait_for_job import MISSING_READS, READ_FAILURES, Missing, current_endpoint, wait


def inventory(*rows):
    return {'total_count': len(rows), 'jobs': [dict(id=i, **row) for i, row in enumerate(rows)]}


class Clock:
    def __init__(self):
        self.now = 0.0
        self.sleeps = []

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


class WaitForJobTest(unittest.TestCase):
    def run_wait(self, reads, timeout=100, interval=10):
        clock = Clock()
        replies = iter(reads)

        def read():
            reply = next(replies)
            if isinstance(reply, Exception):
                raise reply
            return reply
        logs = []
        result = wait(read, 'producer', timeout=timeout, interval=interval,
                      sleep=clock.sleep, clock=clock, log=lambda *a, **k: logs.append(a))
        return result, clock

    def test_waits_until_success(self):
        other = dict(name='producer-2', status='completed', conclusion='failure')
        job, clock = self.run_wait([
            inventory(other, dict(name='producer', status='queued', conclusion=None)),
            inventory(other, dict(name='producer', status='in_progress', conclusion=None)),
            inventory(other, dict(name='producer', status='completed', conclusion='success')),
        ])
        self.assertEqual(job['name'], 'producer')
        self.assertEqual(clock.sleeps, [10, 10])

    def test_immediate_success_does_not_sleep(self):
        _, clock = self.run_wait([inventory(dict(name='producer', status='completed', conclusion='success'))])
        self.assertEqual(clock.sleeps, [])

    def test_rejects_unsuccessful_conclusions(self):
        for conclusion in ('failure', 'cancelled', 'skipped', 'timed_out', None):
            with self.subTest(conclusion=conclusion), self.assertRaisesRegex(RuntimeError, 'finished with'):
                self.run_wait([inventory(dict(name='producer', status='completed', conclusion=conclusion))])

    def test_rejects_ambiguous_name(self):
        row = dict(name='producer', status='completed', conclusion='success')
        with self.assertRaisesRegex(RuntimeError, 'Ambiguous'):
            self.run_wait([inventory(row, row)])

    def test_suffix_or_prefix_is_not_the_job(self):
        with self.assertRaises(Missing):
            self.run_wait([inventory(dict(name='x / producer', status='completed', conclusion='success'),
                                     dict(name='producer (windows)', status='completed', conclusion='success'))] * 20)

    def test_absent_producer_fails_fast(self):
        # A partial rerun can leave the producer out of the current attempt.
        with self.assertRaises(Missing):
            self.run_wait([inventory()] * MISSING_READS, timeout=3600)

    def test_briefly_absent_producer_is_still_awaited(self):
        success = inventory(dict(name='producer', status='completed', conclusion='success'))
        job, _ = self.run_wait([inventory()] * (MISSING_READS - 1) + [success])
        self.assertEqual(job['name'], 'producer')

    def test_times_out(self):
        pending = inventory(dict(name='producer', status='in_progress', conclusion=None))
        with self.assertRaises(TimeoutError):
            self.run_wait([pending] * 20, timeout=30)

    def test_tolerates_transient_read_failures(self):
        success = inventory(dict(name='producer', status='completed', conclusion='success'))
        reads = [GitHubReadError('502')] * (READ_FAILURES - 1) + [success]
        job, _ = self.run_wait(reads)
        self.assertEqual(job['name'], 'producer')

    def test_persistent_read_failure_fails(self):
        with self.assertRaises(GitHubReadError):
            self.run_wait([GitHubReadError('502')] * READ_FAILURES)

    def test_ci_only_endpoint_is_exact_attempt(self):
        with self.assertRaisesRegex(RuntimeError, 'CI only'):
            current_endpoint({})
        self.assertEqual(current_endpoint({'GITHUB_ACTIONS': 'true', 'GITHUB_REPOSITORY': 'o/r',
                                           'GITHUB_RUN_ID': '7', 'GITHUB_RUN_ATTEMPT': '2'}),
                         'repos/o/r/actions/runs/7/attempts/2/jobs')


if __name__ == '__main__':
    unittest.main()
