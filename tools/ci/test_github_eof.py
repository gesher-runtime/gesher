"""Recover only the observed unframed gh EOF; accept only a later complete GET.

The actual reader, inventory fallback and CLI execute. Only subprocess output,
sleep and the deadline clock are supplied at the transport boundary.
"""
import contextlib
import io
import itertools
import json
import subprocess
import unittest
from unittest.mock import call, patch

import github_read as reader
import test_github_read as fixture


EOF = 'unexpected end of JSON input'


def unframed(*, code=1, stderr=EOF + '\n', stdout=b''):
    return subprocess.CompletedProcess(fixture.COMMAND, code,
                                       stdout=stdout, stderr=stderr.encode())


class ExactEofRead(unittest.TestCase):
    def setUp(self):
        self.stderr = io.StringIO()
        self.enterContext(contextlib.redirect_stderr(self.stderr))
        self.run = self.enterContext(patch('github_read.subprocess.run'))
        self.sleep = self.enterContext(patch('github_read.time.sleep'))

    def test_only_the_later_complete_exact_get_is_returned(self):
        complete = {'id': 42444051868, 'run_attempt': 1}
        # The archived diagnostic does not reveal stdout. Exercise both empty
        # and nonempty unframed bytes, including valid but untrusted JSON.
        for ending in ('', '\n', '\r\n'):
            for partial in (b'', b'{"id":', b'{"id": 999, "run_attempt": 99}'):
                with self.subTest(ending=ending, partial=partial):
                    self.run.reset_mock(); self.sleep.reset_mock()
                    self.run.side_effect = [unframed(stderr=EOF + ending, stdout=partial),
                                            fixture.response(complete)]
                    self.assertEqual(reader.get_json(fixture.ENDPOINT), complete)
                    self.assertEqual(self.run.call_args_list,
                        [call(fixture.COMMAND, capture_output=True, timeout=60)] * 2)
                    self.sleep.assert_called_once_with(1)

    def test_other_exit_codes_are_not_an_eof_retry_or_inventory_fallback(self):
        for code in (0, 2, 4, -9):
            for retry in (False, True):
                with self.subTest(code=code, retry=retry):
                    self.run.reset_mock(); self.sleep.reset_mock()
                    self.run.return_value = unframed(code=code)
                    with self.assertRaises(reader.GitHubReadError) as caught:
                        reader.get_json(fixture.ENDPOINT, retry_transient=retry)
                    self.assertNotIsInstance(caught.exception, reader.GitHubTransientError)
                    self.run.assert_called_once(); self.sleep.assert_not_called()

    def test_mixed_or_different_diagnostics_do_not_enter_the_eof_policy(self):
        errors = [EOF + '\nadditional diagnostic', 'additional diagnostic\n' + EOF,
                  EOF + '\n' + EOF, EOF + '\n\n', ' ' + EOF, EOF + ' ',
                  'gh: ' + EOF, EOF.upper(), 'unexpected EOF',
                  EOF + '\nForbidden', 'gh: Forbidden (HTTP 403)\n' + EOF,
                  EOF + '\ngh: HTTP 401\r\n', EOF + ' (HTTP 404)',
                  EOF + '\nstream error: stream ID 1; INTERNAL_ERROR; received from peer']
        for error in errors:
            for retry in (False, True):
                with self.subTest(error=error, retry=retry):
                    self.run.reset_mock(); self.sleep.reset_mock()
                    self.run.return_value = unframed(stderr=error)
                    with self.assertRaises(reader.GitHubReadError) as caught:
                        reader.get_json(fixture.ENDPOINT, retry_transient=retry)
                    self.assertNotIsInstance(caught.exception, reader.GitHubTransientError)
                    self.run.assert_called_once(); self.sleep.assert_not_called()

    def test_http_framing_prevents_the_unframed_eof_exception(self):
        for status in (200, 401, 403, 404, 429, 500):
            for retry in (False, True):
                with self.subTest(status=status, retry=retry):
                    self.run.reset_mock(); self.sleep.reset_mock()
                    self.run.return_value = unframed(stdout=(
                        f'HTTP/2.0 {status}\r\nContent-Type: application/json\r\n\r\n'
                        '{"message":"rejected"}').encode())
                    with self.assertRaises(reader.GitHubReadError) as caught:
                        reader.get_json(fixture.ENDPOINT, retry_transient=retry)
                    self.assertNotIsInstance(caught.exception, reader.GitHubTransientError)
                    self.run.assert_called_once(); self.sleep.assert_not_called()

    def test_successful_framed_malformed_json_is_never_retried(self):
        for raw in (b'', b'{"id":', b'NaN', b'\xff'):
            with self.subTest(raw=raw):
                self.run.reset_mock(); self.sleep.reset_mock()
                self.run.return_value = fixture.response(raw=raw, stderr=EOF + '\n')
                with self.assertRaisesRegex(reader.GitHubReadError, 'invalid JSON'):
                    reader.get_json(fixture.ENDPOINT)
                self.run.assert_called_once(); self.sleep.assert_not_called()

    def test_invalid_http_framing_remains_fatal(self):
        self.run.return_value = unframed(stdout=b'HTTP/2.0 200\r\nBROKEN\r\n\r\n{}')
        with self.assertRaisesRegex(reader.GitHubReadError, 'invalid HTTP headers'):
            reader.get_json(fixture.ENDPOINT)
        self.run.assert_called_once(); self.sleep.assert_not_called()

    def test_exhaustion_has_the_original_four_requests_and_three_waits(self):
        self.run.return_value = unframed(stdout=b'{"partial":')
        with self.assertRaisesRegex(reader.GitHubReadError, r'after 4 attempt\(s\)'):
            reader.get_json(fixture.ENDPOINT)
        self.assertEqual(self.run.call_args_list,
                         [call(fixture.COMMAND, capture_output=True, timeout=60)] * 4)
        self.assertEqual(self.sleep.call_args_list, [call(1), call(2), call(4)])
        self.assertEqual(self.stderr.getvalue().count('CI_GITHUB_READ_RETRY'), 3)

    def test_final_allowed_request_can_return_only_its_own_complete_json(self):
        self.run.side_effect = [unframed(stdout=b'{}')] * 3 + [fixture.response({'complete': True})]
        self.assertEqual(reader.get_json(fixture.ENDPOINT), {'complete': True})
        self.assertEqual(self.run.call_count, 4)
        self.assertEqual(self.sleep.call_args_list, [call(1), call(2), call(4)])

    def test_inventory_caller_gets_one_transient_failure_without_sleep(self):
        self.run.return_value = unframed(stdout=b'{"jobs":[],"total_count":0}')
        with self.assertRaises(reader.GitHubReadError) as caught:
            reader.get_json(fixture.ENDPOINT, retry_transient=False)
        self.assertIsInstance(caught.exception, reader.GitHubTransientError)
        self.run.assert_called_once(); self.sleep.assert_not_called()

    def test_recovery_does_not_retry_a_later_denial_or_invalid_json(self):
        for denied in (fixture.response(code=1, stderr='gh: Forbidden (HTTP 403)'),
                       fixture.response(raw=b'{"id":')):
            with self.subTest(response=denied):
                self.run.reset_mock(); self.sleep.reset_mock()
                self.run.side_effect = [unframed(), denied]
                with self.assertRaises(reader.GitHubReadError):
                    reader.get_json(fixture.ENDPOINT)
                self.assertEqual(self.run.call_count, 2)
                self.sleep.assert_called_once_with(1)

    def test_eof_does_not_extend_the_247_second_transport_deadline(self):
        self.run.return_value = unframed()
        with patch('github_read.time.monotonic',
                   side_effect=itertools.chain([0, 0], itertools.repeat(246))):
            with self.assertRaisesRegex(reader.GitHubReadError, r'after 1 attempt\(s\)'):
                reader.get_json(fixture.ENDPOINT)
        self.run.assert_called_once(); self.sleep.assert_not_called()

    def test_cli_never_publishes_discarded_bytes(self):
        self.run.side_effect = [unframed(stdout=b'{"untrusted":true}'),
                                fixture.response({'verified_transport': True})]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(reader.main([fixture.ENDPOINT]), 0)
        self.assertEqual(json.loads(output.getvalue()), {'verified_transport': True})
        self.assertNotIn('untrusted', output.getvalue())
        self.assertEqual(len(output.getvalue().splitlines()), 1)


if __name__ == '__main__':
    unittest.main()
