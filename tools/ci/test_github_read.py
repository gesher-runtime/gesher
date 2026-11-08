"""Transport recovery must preserve exact reads and every receipt rejection."""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import unittest
from unittest.mock import call, patch

from github_read import GitHubReadError, get_json, main


ENDPOINT = 'repos/gesher-runtime/gesher/actions/runs/42444051868/attempts/1/jobs?per_page=100'
COMMAND = ['gh', 'api', '--method', 'GET', '--', ENDPOINT]
ROOT = Path(__file__).resolve().parents[2]


def response(value=None, *, code=0, stderr='', raw=None):
    return subprocess.CompletedProcess(
        COMMAND, code, stdout=json.dumps(value).encode() if raw is None else raw,
        stderr=stderr.encode())


class GitHubReadTest(unittest.TestCase):
    def setUp(self):
        self.stderr = io.StringIO()
        self.enterContext(contextlib.redirect_stderr(self.stderr))
        self.run = self.enterContext(patch('github_read.subprocess.run'))
        self.sleep = self.enterContext(patch('github_read.time.sleep'))

    def test_success_preserves_the_exact_get_and_parses_json(self):
        value = {'jobs': [{'run_id': 42444051868, 'run_attempt': 1}], 'total_count': 1}
        self.run.return_value = response(value)
        self.assertEqual(get_json(ENDPOINT), value)
        self.run.assert_called_once_with(COMMAND, capture_output=True, timeout=60)
        self.sleep.assert_not_called()

    def test_each_observed_http_status_recovers_without_changing_identity(self):
        for status in (502, 503, 504):
            with self.subTest(status=status):
                self.run.reset_mock()
                self.sleep.reset_mock()
                self.run.side_effect = [response(code=1, stderr=f'gh: Server Error (HTTP {status})'),
                                        response({'id': 42444051868, 'run_attempt': 1})]
                self.assertEqual(get_json(ENDPOINT), {'id': 42444051868, 'run_attempt': 1})
                self.assertEqual(self.run.call_args_list,
                                 [call(COMMAND, capture_output=True, timeout=60)] * 2)
                self.sleep.assert_called_once_with(1)

    def test_observed_http2_cancel_with_or_without_get_prefix_recovers(self):
        for prefix in ('', 'Get "https://api.github.com/' + ENDPOINT + '": ', 'gh: '):
            with self.subTest(prefix=prefix):
                self.run.reset_mock()
                self.sleep.reset_mock()
                self.run.side_effect = [response(code=1, stderr=prefix +
                    'stream error: stream ID 1; CANCEL; received from peer\r\n'), response([])]
                self.assertEqual(get_json(ENDPOINT), [])
                self.assertEqual(self.run.call_count, 2)
                self.sleep.assert_called_once_with(1)

    def test_four_attempts_exhaust_with_exact_bounded_backoff(self):
        self.run.side_effect = [response(code=1, stderr=f'gh: Server Error (HTTP {status})')
                                for status in (502, 503, 504, 502)]
        with self.assertRaisesRegex(GitHubReadError, r'after 4 attempt\(s\)'):
            get_json(ENDPOINT)
        self.assertEqual(self.run.call_args_list,
                         [call(COMMAND, capture_output=True, timeout=60)] * 4)
        self.assertEqual(self.sleep.call_args_list, [call(1), call(2), call(4)])
        self.assertEqual(self.stderr.getvalue().count('CI_GITHUB_READ_RETRY'), 3)

    def test_final_allowed_attempt_can_succeed(self):
        self.run.side_effect = [response(code=1, stderr='gh: Server Error (HTTP 502)')] * 3 + [
            response({'complete': True})]
        self.assertEqual(get_json(ENDPOINT), {'complete': True})
        self.assertEqual(self.run.call_count, 4)
        self.assertEqual(self.sleep.call_args_list, [call(1), call(2), call(4)])

    def test_permanent_and_unobserved_errors_fail_without_retry(self):
        errors = [f'gh: rejected (HTTP {status})' for status in (400, 401, 403, 404, 408, 429, 500, 501)]
        errors += ['connection reset by peer', 'context deadline exceeded',
                   'HTTP/2 INTERNAL_ERROR', 'CANCEL',
                   'stream error: stream ID 1; INTERNAL_ERROR; received from peer',
                   'stream error: stream ID 1; REFUSED_STREAM; received from peer',
                   'stream error: stream ID 0; CANCEL; received from peer',
                   'stream error: stream ID 1; CANCEL; unexpected trailing message',
                   'gh: rejected (HTTP 403)\nstream error: stream ID 1; CANCEL; received from peer',
                   'gh: rejected (HTTP 401)\ngh: Server Error (HTTP 502)', '']
        for error in errors:
            with self.subTest(error=error):
                self.run.reset_mock()
                self.sleep.reset_mock()
                self.run.side_effect = None
                self.run.return_value = response({'must_not_be_accepted': True}, code=1, stderr=error)
                with self.assertRaises(GitHubReadError):
                    get_json(ENDPOINT)
                self.assertEqual(self.run.call_count, 1)
                self.sleep.assert_not_called()

    def test_a_permanent_error_after_one_transient_stops_immediately(self):
        self.run.side_effect = [response(code=1, stderr='gh: Server Error (HTTP 502)'),
                                response(code=1, stderr='gh: Forbidden (HTTP 403)')]
        with self.assertRaisesRegex(GitHubReadError, r'after 2 attempt\(s\)'):
            get_json(ENDPOINT)
        self.assertEqual(self.run.call_count, 2)
        self.sleep.assert_called_once_with(1)

    def test_invalid_or_empty_json_and_encoding_never_retry(self):
        for raw in (b'', b'<html>502 Bad Gateway</html>', b'{"id":', b'\xff',
                    b'NaN', b'Infinity', b'-Infinity'):
            with self.subTest(raw=raw):
                self.run.reset_mock()
                self.sleep.reset_mock()
                self.run.return_value = response(raw=raw)
                with self.assertRaisesRegex(GitHubReadError, 'invalid JSON'):
                    get_json(ENDPOINT)
                self.assertEqual(self.run.call_count, 1)
                self.sleep.assert_not_called()

    def test_invalid_json_after_recovery_is_not_another_transport_retry(self):
        self.run.side_effect = [response(code=1, stderr='gh: Server Error (HTTP 504)'),
                                response(raw=b'not JSON')]
        with self.assertRaisesRegex(GitHubReadError, 'invalid JSON'):
            get_json(ENDPOINT)
        self.assertEqual(self.run.call_count, 2)
        self.sleep.assert_called_once_with(1)

    def test_launch_failure_and_process_timeout_fail_without_retry(self):
        for error in (FileNotFoundError('gh unavailable'), subprocess.TimeoutExpired(COMMAND, 60)):
            with self.subTest(error=error):
                self.run.reset_mock()
                self.sleep.reset_mock()
                self.run.side_effect = error
                with self.assertRaises(GitHubReadError):
                    get_json(ENDPOINT)
                self.assertEqual(self.run.call_count, 1)
                self.sleep.assert_not_called()

    def test_endpoint_is_not_rewritten_or_interpreted_as_an_option(self):
        endpoint = '--method=POST'
        self.run.return_value = response({})
        self.assertEqual(get_json(endpoint), {})
        self.run.assert_called_once_with(['gh', 'api', '--method', 'GET', '--', endpoint],
                                         capture_output=True, timeout=60)

    def test_cli_outputs_only_complete_json_with_retry_diagnostics_on_stderr(self):
        self.run.side_effect = [response(code=1, stderr='gh: Server Error (HTTP 502)'),
                                response({'tag': '\u05d2\u05e9\u05e8'})]
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            self.assertEqual(main([ENDPOINT]), 0)
        self.assertEqual(json.loads(stdout.getvalue()), {'tag': '\u05d2\u05e9\u05e8'})
        self.assertEqual(len(stdout.getvalue().splitlines()), 1)
        self.assertIn('CI_GITHUB_READ_RETRY', self.stderr.getvalue())

    def test_cli_failure_has_no_json_stdout_and_returns_one(self):
        self.run.return_value = response({'partial': True}, code=4, stderr='gh: Forbidden (HTTP 403)')
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            self.assertEqual(main([ENDPOINT]), 1)
        self.assertEqual(stdout.getvalue(), '')
        self.assertIn('gh exit 4', self.stderr.getvalue())
        self.assertIn('Forbidden (HTTP 403)', self.stderr.getvalue())
        self.sleep.assert_not_called()


class ReceiptIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.repo = 'gesher-runtime/gesher'
        self.head = 'b' * 40
        self.checkout = 'a' * 40
        self.env = dict(GITHUB_ACTIONS='true', GITHUB_REPOSITORY=self.repo,
            GITHUB_RUN_ID='7', GITHUB_RUN_ATTEMPT='2', GITHUB_REPOSITORY_ID='8',
            GITHUB_SHA=self.checkout, GITHUB_EVENT_PATH='unused-event.json',
            GITHUB_EVENT_NAME='pull_request', GITHUB_WORKFLOW='ci')
        self.event = {'pull_request': {'head': {'sha': self.head, 'repo': {'id': 8}}}}
        self.run_value = {'id': 7, 'run_attempt': 2, 'head_sha': self.head, 'name': 'ci',
            'repository': {'id': 8, 'full_name': self.repo}, 'head_repository': {'id': 8}}
        self.jobs = {'total_count': 1, 'jobs': [{'id': 9, 'name': 'build',
            'conclusion': 'success', 'head_sha': self.head, 'run_id': 7, 'run_attempt': 2,
            'started_at': '2026-11-08T03:49:02Z', 'completed_at': '2026-11-08T03:50:57Z'}]}
        self.inventory = {'total_count': 1, 'artifacts': [{'id': 10, 'name': 'windows-x64',
            'expired': False, 'size_in_bytes': 100, 'digest': 'sha256:' + 'c' * 64,
            'created_at': '2026-11-08T03:50:35Z', 'workflow_run': {'id': 7,
                'head_sha': self.head, 'repository_id': 8, 'head_repository_id': 8}}]}
        self.endpoints = [f'repos/{self.repo}/actions/runs/7',
            f'repos/{self.repo}/actions/runs/7/attempts/2/jobs?per_page=100',
            f'repos/{self.repo}/actions/runs/7/artifacts?per_page=100']
        self.commands = []
        self.downloads = []
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.enterContext(contextlib.redirect_stderr(io.StringIO()))
        self.enterContext(patch.dict(os.environ, self.env, clear=True))
        self.enterContext(patch.object(sys, 'argv', ['artifact_receipt.py', '--name', 'windows-x64',
            '--producer', 'build', '--destination', 'supplier']))
        self.enterContext(patch('pathlib.Path.read_text', return_value=json.dumps(self.event)))
        self.enterContext(patch('subprocess.check_output', return_value=self.checkout + '\n'))
        self.sleep = self.enterContext(patch('github_read.time.sleep'))
        self.invoke = self.enterContext(patch('subprocess.run', side_effect=self._invoke))

    def _invoke(self, command, **kwargs):
        if command[0] == 'gh':
            self.commands.append(command)
            self.assertEqual(command[:5], ['gh', 'api', '--method', 'GET', '--'])
            return self.responses.pop(0)
        self.downloads.append(command)
        self.assertEqual(command[:2], [sys.executable, 'tools/surface/download-verified-artifact.py'])
        self.assertEqual(kwargs, {'check': True})
        return subprocess.CompletedProcess(command, 0)

    def _receipt(self):
        runpy.run_path(str(ROOT / 'tools/ci/artifact_receipt.py'), run_name='__main__')

    def test_actual_receipt_recovers_and_preserves_paths_and_verified_download(self):
        self.responses = [response(self.run_value), response(code=1, stderr='gh: Server Error (HTTP 502)'),
                          response(self.jobs), response(self.inventory)]
        self._receipt()
        self.assertEqual([command[-1] for command in self.commands],
                         [self.endpoints[0], self.endpoints[1], self.endpoints[1], self.endpoints[2]])
        self.sleep.assert_called_once_with(1)
        self.assertEqual(self.downloads, [[sys.executable, 'tools/surface/download-verified-artifact.py',
            '--id', '10', '--digest', 'sha256:' + 'c' * 64, '--destination', 'supplier']])

    def test_successful_transport_never_retries_or_accepts_semantic_rejections(self):
        cases = []
        wrong_run = copy.deepcopy(self.run_value); wrong_run['head_sha'] = 'd' * 40
        cases.append(('source', wrong_run, self.jobs, self.inventory, 1))
        for field, value in (('conclusion', 'failure'), ('run_attempt', 1), ('head_sha', 'd' * 40)):
            jobs = copy.deepcopy(self.jobs); jobs['jobs'][0][field] = value
            cases.append((field, self.run_value, jobs, self.inventory, 2))
        jobs = copy.deepcopy(self.jobs); jobs['jobs'].append(copy.deepcopy(jobs['jobs'][0]))
        cases.append(('ambiguous producer', self.run_value, jobs, self.inventory, 2))
        for field, value in (('digest', 'sha256:wrong'), ('expired', True),
                             ('created_at', '2026-11-08T03:48:00Z')):
            inventory = copy.deepcopy(self.inventory); inventory['artifacts'][0][field] = value
            cases.append((field, self.run_value, self.jobs, inventory, 3))
        inventory = copy.deepcopy(self.inventory)
        inventory['artifacts'][0]['workflow_run']['head_sha'] = 'd' * 40
        cases.append(('artifact source', self.run_value, self.jobs, inventory, 3))
        for label, run, jobs, inventory, expected_calls in cases:
            with self.subTest(label=label):
                self.commands.clear()
                self.downloads.clear()
                self.sleep.reset_mock()
                self.responses = [response(run), response(jobs), response(inventory)]
                with self.assertRaises(RuntimeError):
                    self._receipt()
                self.assertEqual(len(self.commands), expected_calls)
                self.assertEqual(self.downloads, [])
                self.sleep.assert_not_called()


if __name__ == '__main__':
    unittest.main()
