"""Exercise the pinned-source downloader with isolated HTTP responses."""
import base64
import binascii
import contextlib
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock
import urllib.error
import urllib.request

SPEC = importlib.util.spec_from_file_location(
    'prepare_sources', Path(__file__).with_name('prepare_sources.py'))
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)
PAYLOAD = b'isolated transport fixture, never compiled as parser source\n'
URL = 'https://android.googlesource.com/platform/bionic/+/fixed-commit/libc/file.c?format=TEXT'


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests += 1
        replies = self.server.replies
        status, body, length = replies.pop(0) if len(replies) > 1 else replies[0]
        self.send_response(status)
        self.send_header('Content-Length', str(len(body) if length is None else length))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


class PinnedSourceDownload(unittest.TestCase):
    def setUp(self):
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.destination = self.directory / 'aosp' / 'bionic' / 'libc' / 'file.c'
        manifest = dict(repository='https://android.googlesource.com/platform/bionic',
                        commit='fixed-commit', files={'libc/file.c': hashlib.sha256(PAYLOAD).hexdigest()})
        (self.directory / 'sources.json').write_text(json.dumps(manifest))
        self.enterContext(mock.patch.object(PREPARE, '__file__', str(self.directory / 'prepare_sources.py')))
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.requests = 0
        self.server.replies = [(200, base64.b64encode(PAYLOAD), None)]
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(poll_interval=0.01), daemon=True)
        self.thread.start()
        self.addCleanup(self.close)
        urlopen = urllib.request.urlopen
        self.urlopen = self.enterContext(mock.patch.object(PREPARE.urllib.request, 'urlopen',
            side_effect=lambda url, timeout: urlopen(f'http://127.0.0.1:{self.server.server_port}/source', timeout=timeout)))
        self.sleep = self.enterContext(mock.patch.object(PREPARE.time, 'sleep'))
        self.enterContext(contextlib.redirect_stderr(io.StringIO()))
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def prepare(self, fetch=True):
        argv = ['prepare_sources.py', str(self.directory / 'aosp')]
        if fetch:
            argv.append('--fetch')
        with mock.patch.object(PREPARE.sys, 'argv', argv):
            PREPARE.main()

    def test_success_keeps_pinned_url_timeout_and_verified_bytes(self):
        self.prepare()
        self.assertEqual(self.destination.read_bytes(), PAYLOAD)
        self.urlopen.assert_called_once_with(URL, timeout=45)
        self.sleep.assert_not_called()

    def test_temporary_http_failures_then_success(self):
        for status in (500, 502, 503, 504):
            with self.subTest(status=status):
                self.server.replies = [(status, b'unavailable', None), (200, base64.b64encode(PAYLOAD), None)]
                before = self.server.requests
                self.prepare()
                self.assertEqual(self.destination.read_bytes(), PAYLOAD)
                self.assertEqual(self.server.requests - before, 2)
                self.destination.unlink()

    def test_exhausted_retries_leave_no_destination(self):
        self.server.replies = [(503, b'unavailable', None)]
        with self.assertRaisesRegex(RuntimeError, 'after 4 attempts') as caught:
            self.prepare()
        self.assertIsInstance(caught.exception.__cause__, urllib.error.HTTPError)
        self.assertEqual(self.server.requests, 4)
        self.assertEqual(self.sleep.call_args_list, [mock.call(1), mock.call(2), mock.call(4)])
        self.assertFalse(self.destination.exists())

    def test_permanent_http_errors_fail_immediately(self):
        for status in (401, 403, 404):
            with self.subTest(status=status):
                self.server.replies = [(status, b'failure', None)]
                before = self.server.requests
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    self.prepare()
                self.assertEqual(caught.exception.code, status)
                self.assertEqual(self.server.requests - before, 1)
        self.sleep.assert_not_called()
        self.assertFalse(self.destination.exists())

    def test_partial_response_is_discarded_before_retry(self):
        encoded = base64.b64encode(PAYLOAD)
        self.server.replies = [(200, encoded[:8], len(encoded)), (200, encoded, None)]
        self.prepare()
        self.assertEqual(self.destination.read_bytes(), PAYLOAD)
        self.assertEqual(self.server.requests, 2)

    def test_connection_failure_and_timeout_are_retried(self):
        real_transport = self.urlopen.side_effect
        for error in (urllib.error.URLError('connection reset'), TimeoutError('timed out')):
            with self.subTest(error=error):
                calls = []
                def transport(url, timeout):
                    calls.append(url)
                    if len(calls) == 1:
                        raise error
                    return real_transport(url, timeout)
                self.urlopen.side_effect = transport
                self.prepare()
                self.assertEqual(self.destination.read_bytes(), PAYLOAD)
                self.assertEqual(len(calls), 2)
                self.destination.unlink()

    def test_invalid_encoding_fails_without_retry_or_publication(self):
        self.server.replies = [(200, b'not base64!', None)]
        with self.assertRaises(binascii.Error):
            self.prepare()
        self.assertEqual(self.server.requests, 1)
        self.sleep.assert_not_called()
        self.assertFalse(self.destination.exists())

    def test_wrong_digest_fails_without_retry_or_publication(self):
        self.server.replies = [(200, base64.b64encode(b'changed content'), None)]
        with self.assertRaisesRegex(SystemExit, 'upstream source digest mismatch'):
            self.prepare()
        self.assertEqual(self.server.requests, 1)
        self.sleep.assert_not_called()
        self.assertFalse(self.destination.exists())

    def test_existing_changed_source_is_rejected_without_refetching(self):
        self.destination.parent.mkdir(parents=True)
        self.destination.write_bytes(b'changed content')
        with self.assertRaisesRegex(SystemExit, 'missing or changed pinned source'):
            self.prepare()
        self.urlopen.assert_not_called()
        self.assertEqual(self.destination.read_bytes(), b'changed content')

    def test_verified_cache_and_missing_offline_source_do_not_use_network(self):
        self.destination.parent.mkdir(parents=True)
        self.destination.write_bytes(PAYLOAD)
        self.prepare()
        self.prepare(fetch=False)
        self.destination.unlink()
        with self.assertRaisesRegex(SystemExit, 'missing or changed pinned source'):
            self.prepare(fetch=False)
        self.urlopen.assert_not_called()


if __name__ == '__main__':
    unittest.main()
