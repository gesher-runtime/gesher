"""Exercise the SQLite fixture's real downloader with isolated HTTP responses."""
import base64
import binascii
import contextlib
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import io
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock
import urllib.error
import urllib.request

SPEC = importlib.util.spec_from_file_location('sqlite_paths', Path(__file__).parents[1] / 'test_sqlite_paths.py')
SQLITE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SQLITE)
PAYLOAD = b'isolated transport fixture, never compiled as SQLite\n'
DIGEST = hashlib.sha256(PAYLOAD).hexdigest()


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


class SQLiteSourceDownload(unittest.TestCase):
    def setUp(self):
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.requests = 0
        self.server.replies = [(200, base64.b64encode(PAYLOAD), None)]
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(poll_interval=0.01), daemon=True)
        self.thread.start()
        self.addCleanup(self.close)
        urlopen = urllib.request.urlopen
        self.urlopen = self.enterContext(mock.patch.object(SQLITE.urllib.request, 'urlopen',
            side_effect=lambda url, timeout: urlopen(f'http://127.0.0.1:{self.server.server_port}/source', timeout=timeout)))
        self.sleep = self.enterContext(mock.patch.object(SQLITE.time, 'sleep'))
        self.enterContext(contextlib.redirect_stderr(io.StringIO()))

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def source(self):
        return SQLITE.source_bytes('sqlite3.c', DIGEST)

    def test_success_keeps_exact_pinned_url_and_timeout(self):
        self.assertEqual(self.source(), PAYLOAD)
        self.urlopen.assert_called_once_with(
            'https://android.googlesource.com/platform/external/sqlite/+/refs/tags/'
            'android-16.0.0_r4/dist/sqlite-autoconf-3500400/sqlite3.c?format=TEXT', timeout=60)
        self.sleep.assert_not_called()

    def test_transient_http_errors_then_success(self):
        for status in (500, 502, 503, 504):
            with self.subTest(status=status):
                self.server.replies = [(status, b'unavailable', None), (200, base64.b64encode(PAYLOAD), None)]
                before = self.server.requests
                self.assertEqual(self.source(), PAYLOAD)
                self.assertEqual(self.server.requests - before, 2)

    def test_exhausted_retries_fail_with_bounded_backoff(self):
        self.server.replies = [(503, b'unavailable', None)]
        with self.assertRaisesRegex(RuntimeError, 'after 4 attempts') as caught:
            self.source()
        self.assertIsInstance(caught.exception.__cause__, urllib.error.HTTPError)
        self.assertEqual(self.server.requests, 4)
        self.assertEqual(self.sleep.call_args_list, [mock.call(1), mock.call(2), mock.call(4)])

    def test_permanent_http_errors_fail_immediately(self):
        for status in (401, 403, 404):
            with self.subTest(status=status):
                self.server.replies = [(status, b'failure', None)]
                before = self.server.requests
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    self.source()
                self.assertEqual(caught.exception.code, status)
                self.assertEqual(self.server.requests - before, 1)
        self.sleep.assert_not_called()

    def test_partial_response_is_discarded_before_retry(self):
        encoded = base64.b64encode(PAYLOAD)
        self.server.replies = [(200, encoded[:8], len(encoded)), (200, encoded, None)]
        self.assertEqual(self.source(), PAYLOAD)
        self.assertEqual(self.server.requests, 2)

    def test_network_failure_and_timeout_are_retried(self):
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
                self.assertEqual(self.source(), PAYLOAD)
                self.assertEqual(len(calls), 2)

    def test_invalid_base64_is_not_retried(self):
        self.server.replies = [(200, b'not base64!', None)]
        with self.assertRaises(binascii.Error):
            self.source()
        self.assertEqual(self.server.requests, 1)
        self.sleep.assert_not_called()

    def test_wrong_digest_is_not_retried(self):
        self.server.replies = [(200, base64.b64encode(b'wrong content'), None)]
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            self.source()
        self.assertEqual(self.server.requests, 1)
        self.sleep.assert_not_called()

    def test_local_sources_still_require_the_digest_without_network(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'sqlite3.c').write_bytes(PAYLOAD)
            self.assertEqual(SQLITE.source_bytes('sqlite3.c', DIGEST, path), PAYLOAD)
            (path / 'sqlite3.c').write_bytes(b'wrong')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                SQLITE.source_bytes('sqlite3.c', DIGEST, path)
        self.urlopen.assert_not_called()


if __name__ == '__main__':
    unittest.main()
