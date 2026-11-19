"""Quota recovery uses live headers; failed authorization never becomes evidence."""
import contextlib
from email.message import Message
from email.utils import formatdate
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
from unittest.mock import patch
import urllib.error
import zipfile

import github_read as reader

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('quota_archive', ROOT / 'tools/surface/download-verified-artifact.py')
archive = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(archive)
ENDPOINT = 'repos/gesher-runtime/gesher/actions/runs/7'
NOW = 1795098000
PRIMARY = b'{"message":"API rate limit exceeded for installation."}'
SECONDARY = b'{"message":"You have exceeded a secondary rate limit. Please wait."}'


def headers(wait=9):
    return {'date': formatdate(NOW, usegmt=True), 'x-ratelimit-remaining': '0',
            'x-ratelimit-reset': str(NOW + wait)}


def reply(status=200, body=b'{"id":7}', fields=None, stderr=None):
    lines = [f'HTTP/2.0 {status}'] + [key + ': ' + value for key, value in (fields or {}).items()]
    return subprocess.CompletedProcess([], 0 if status == 200 else 1,
        stdout=('\r\n'.join(lines) + '\r\n\r\n').encode() + body,
        stderr=(stderr if stderr is not None else '' if status == 200 else f'gh: failure (HTTP {status})').encode())


class QuotaReads(unittest.TestCase):
    def setUp(self):
        self.logs = io.StringIO()
        self.enterContext(contextlib.redirect_stderr(self.logs))
        self.clock = 0
        self.enterContext(patch.object(reader.time, 'monotonic', side_effect=lambda: self.clock))
        self.sleep = self.enterContext(patch.object(reader.time, 'sleep', side_effect=self.advance))
        self.run = self.enterContext(patch.object(reader.subprocess, 'run'))

    def advance(self, seconds):
        self.clock += seconds

    def test_primary_reset_and_retry_after_are_both_honored(self):
        fields = dict(headers(), **{'retry-after': '12'})
        self.run.side_effect = [reply(403, PRIMARY, fields), reply()]
        self.assertEqual(reader.get_json(ENDPOINT), {'id': 7})
        self.assertEqual([c.args for c in self.sleep.call_args_list], [(12,)])
        self.assertEqual([c.args[0] for c in self.run.call_args_list],
            [['gh', 'api', '--method', 'GET', '--include', '--', ENDPOINT]] * 2)
        self.assertIn('action=wait', self.logs.getvalue())

    def test_server_date_prevents_fast_runner_clock_early_retry(self):
        self.run.side_effect = [reply(429, PRIMARY, headers()), reply()]
        with patch.object(reader.time, 'time', return_value=NOW + 3600):
            self.assertEqual(reader.get_json(ENDPOINT), {'id': 7})
        self.assertEqual([c.args for c in self.sleep.call_args_list], [(10,)])

    def test_secondary_without_retry_after_waits_a_minute_then_stops_within_budget(self):
        self.run.side_effect = [reply(403, SECONDARY), reply(403, SECONDARY)]
        with self.assertRaisesRegex(reader.GitHubReadError, 'bounded'):
            reader.get_json(ENDPOINT)
        self.assertEqual(self.run.call_count, 2)
        self.assertEqual([c.args for c in self.sleep.call_args_list], [(60,)])
        self.assertEqual(self.clock, 60)

    def test_secondary_retry_after_can_recover(self):
        self.run.side_effect = [reply(429, SECONDARY, {'retry-after': '8'}), reply()]
        self.assertEqual(reader.get_json(ENDPOINT), {'id': 7})
        self.sleep.assert_called_once_with(8)

    def test_distant_reset_and_cumulative_wait_fail_without_early_retry(self):
        self.run.return_value = reply(403, PRIMARY, headers(3600))
        with self.assertRaisesRegex(reader.GitHubReadError, 'bounded'):
            reader.get_json(ENDPOINT)
        self.run.assert_called_once()
        self.sleep.assert_not_called()
        self.assertIn('reset=' + str(NOW + 3600), self.logs.getvalue())
        self.run.reset_mock()
        self.run.side_effect = [reply(403, PRIMARY, headers(59)), reply(403, PRIMARY, headers(69))]
        with self.assertRaisesRegex(reader.GitHubReadError, 'bounded'):
            reader.get_json(ENDPOINT)
        self.assertEqual(self.run.call_count, 2)
        self.assertEqual([c.args for c in self.sleep.call_args_list], [(60,)])

    def test_rate_wait_does_not_extend_original_247_second_read_envelope(self):
        responses = [reply(502), reply(503), reply(403, PRIMARY, headers(69))]
        def invoke(*args, **kwargs):
            self.assertLessEqual(kwargs['timeout'], 60)
            self.advance(59)
            return responses.pop(0)
        self.run.side_effect = invoke
        with self.assertRaisesRegex(reader.GitHubReadError, 'bounded'):
            reader.get_json(ENDPOINT)
        self.assertEqual(self.run.call_count, 3)
        self.assertEqual([c.args for c in self.sleep.call_args_list], [(1,), (2,)])
        self.assertLess(self.clock, 247)

    def test_auth_errors_and_quota_looking_headers_are_not_transient(self):
        for status in (401, 403, 404, 429):
            for body in (b'{"message":"Resource not accessible by integration"}', b'not JSON'):
                with self.subTest(status=status, body=body):
                    self.run.reset_mock(); self.sleep.reset_mock()
                    self.run.return_value = reply(status, body, headers())
                    with self.assertRaises(reader.GitHubReadError): reader.get_json(ENDPOINT)
                    self.run.assert_called_once(); self.sleep.assert_not_called()

    def test_malformed_missing_or_duplicate_quota_headers_fail_closed(self):
        cases = [{}, dict(headers(), **{'date': 'not a date'}),
                 dict(headers(), **{'date': 'Thu, 19 Nov 2026 14:20:00'}),
                 dict(headers(), **{'x-ratelimit-reset': 'NaN'}),
                 dict(headers(), **{'x-ratelimit-remaining': '1'}),
                 dict(headers(), **{'retry-after': '-1'}),
                 dict(headers(), **{'retry-after': '1.5'})]
        for fields in cases:
            with self.subTest(fields=fields):
                self.run.reset_mock(); self.sleep.reset_mock()
                self.run.return_value = reply(403, PRIMARY, fields)
                with self.assertRaises(reader.GitHubReadError): reader.get_json(ENDPOINT)
                self.run.assert_called_once(); self.sleep.assert_not_called()
        value = reply(403, PRIMARY, headers())
        value.stdout = value.stdout.replace(b'\r\n\r\n', b'\r\nX-RateLimit-Remaining: 0\r\n\r\n')
        self.run.return_value = value
        with self.assertRaisesRegex(reader.GitHubReadError, 'duplicate quota headers'):
            reader.get_json(ENDPOINT)
        self.sleep.assert_not_called()

    def test_conflicting_status_invalid_recovered_json_and_false_success_fail(self):
        self.run.return_value = reply(403, PRIMARY, headers(), stderr='gh: denied (HTTP 401)')
        with self.assertRaisesRegex(reader.GitHubReadError, 'conflicting'):
            reader.get_json(ENDPOINT)
        self.sleep.assert_not_called()
        self.run.reset_mock()
        self.run.side_effect = [reply(403, PRIMARY, headers()), reply(body=b'NaN')]
        with self.assertRaisesRegex(reader.GitHubReadError, 'invalid JSON'):
            reader.get_json(ENDPOINT)
        self.assertEqual(self.run.call_count, 2)
        self.run.side_effect = None
        value = reply(403, PRIMARY, headers()); value.returncode = 0
        self.run.return_value = value
        with self.assertRaisesRegex(reader.GitHubReadError, 'successful HTTP status'):
            reader.get_json(ENDPOINT)

    def test_standalone_policy_matches_reader_for_valid_and_adversarial_responses(self):
        variants = [(403, PRIMARY, headers()), (429, PRIMARY, headers(3600)),
            (403, SECONDARY, {}), (403, SECONDARY, {'retry-after': '4'}),
            (403, PRIMARY, {}), (401, PRIMARY, headers()),
            (403, b'[]', headers()), (403, b'{"message":"Forbidden"}', headers()),
            (403, PRIMARY, dict(headers(), **{'retry-after': 'bad'})),
            (403, PRIMARY + b' ' * 16384, headers())]
        for status, body, fields in variants:
            with self.subTest(status=status, body=body[:80], fields=fields):
                self.assertEqual(reader.rate_limit_info(status, fields, body),
                                 archive.rate_limit_info(status, fields, body))


class QuotaArchive(unittest.TestCase):
    def test_real_extraction_recovers_only_for_verified_api_quota(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = io.BytesIO()
            with zipfile.ZipFile(payload, 'w') as package:
                package.writestr('nested/actual.txt', 'verified data')
            data = payload.getvalue()
            digest = 'sha256:' + hashlib.sha256(data).hexdigest()
            calls, storage = [], []
            limit = urllib.error.HTTPError('https://api.github.com/exact', 403, 'PRIVATE_ERROR', headers(), io.BytesIO(PRIMARY))
            def api(request, timeout):
                calls.append(request)
                self.assertEqual(request.get_header('Authorization'), 'Bearer TEST_TOKEN_NEVER_LOG')
                if len(calls) == 1: raise limit
                raise urllib.error.HTTPError(request.full_url, 302, '',
                    {'Location': 'https://storage.test/file?sig=PRIVATE_SIGNED_URL'}, io.BytesIO())
            def stored(location, timeout):
                storage.append(location)
                self.assertIsInstance(location, str)
                return io.BytesIO(data)
            def opener(handler):
                return types.SimpleNamespace(open=api if handler is archive.NoRedirect else stored)
            logs = io.StringIO()
            with patch.dict(os.environ, {'GITHUB_ACTIONS': 'true', 'GITHUB_REPOSITORY': 'owner/repo', 'GH_TOKEN': 'TEST_TOKEN_NEVER_LOG'}), \
                    patch.object(archive.urllib.request, 'build_opener', side_effect=opener), \
                    patch.object(archive.time, 'sleep') as sleep, contextlib.redirect_stderr(logs):
                archive.download_verified('12', digest, root / 'output')
            self.assertEqual((root / 'output/nested/actual.txt').read_text(), 'verified data')
            self.assertEqual(len(calls), 2)
            self.assertEqual(len(storage), 1)
            sleep.assert_called_once_with(10)
            self.assertTrue(limit.closed)
            for private in ('TEST_TOKEN_NEVER_LOG', 'PRIVATE_SIGNED_URL', 'PRIVATE_ERROR'):
                self.assertNotIn(private, logs.getvalue())

    def test_api_denial_long_reset_and_duplicate_headers_publish_nothing(self):
        for kind in ('denial', 'long reset', 'duplicate', 'malformed'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                fields = headers(3600 if kind == 'long reset' else 9)
                body = PRIMARY
                if kind == 'denial': body = b'{"message":"Resource not accessible by integration"}'
                if kind == 'malformed': fields['x-ratelimit-reset'] = 'unknown'
                if kind == 'duplicate':
                    duplicate = Message()
                    for key, value in fields.items(): duplicate[key] = value
                    duplicate['X-RateLimit-Remaining'] = '0'; fields = duplicate
                error = urllib.error.HTTPError('https://api.github.com/exact', 403, 'PRIVATE', fields, io.BytesIO(body))
                opener = types.SimpleNamespace(open=lambda *a, **k: (_ for _ in ()).throw(error))
                with patch.dict(os.environ, {'GITHUB_ACTIONS': 'true', 'GITHUB_REPOSITORY': 'owner/repo', 'GH_TOKEN': 'TEST'}), \
                        patch.object(archive.urllib.request, 'build_opener', return_value=opener) as build, \
                        patch.object(archive.time, 'sleep') as sleep, contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(archive.DownloadError):
                        archive.download_verified('12', 'sha256:' + 'a' * 64, Path(temporary) / 'output')
                self.assertEqual(build.call_count, 1)
                sleep.assert_not_called()
                self.assertFalse((Path(temporary) / 'output').exists())
                self.assertTrue(error.closed)


if __name__ == '__main__':
    unittest.main()
