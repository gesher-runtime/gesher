"""Exercise the real XSD dependency downloader against an isolated HTTP server."""
import hashlib
import importlib.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    'framework_xsdc_builder', Path(__file__).parents[1] / 'framework' / 'build.py')
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)

# Transport-only fixture bytes, never used as Java classes. Real pinned AOSP
# generation separately consumes the production Commons CLI 1.2 digest.
PAYLOAD = b'isolated XSD dependency transport fixture\n'


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        responses = self.server.responses.get(self.path, [(404, b'missing', None)])
        status, body, length = responses.pop(0) if len(responses) > 1 else responses[0]
        self.send_response(status)
        self.send_header('Content-Length', str(len(body) if length is None else length))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


class XsdcDependencyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.builder = BUILD.Builder(SimpleNamespace(
            aosp=str(self.root / 'aosp'), out=str(self.root / 'out'),
            java_home=str(self.root / 'java')))
        self.jar_dir = self.root / 'out' / 'xsdc'
        self.classes = self.jar_dir / 'classes'
        self.classes.mkdir(parents=True)
        # An existing compile cache must not bypass validation of its runtime jar.
        self.marker = self.classes / 'compile-cache-marker'
        self.marker.write_bytes(b'unchanged compiled tool')
        self.jar = self.jar_dir / 'commons-cli-1.2.jar'
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.responses = {}
        self.server.requests = []
        self.thread = threading.Thread(
            target=lambda: self.server.serve_forever(poll_interval=0.01), daemon=True)
        self.thread.start()
        self.addCleanup(self.close_server)
        origin = f'http://127.0.0.1:{self.server.server_port}'
        self.builder.XSDC_COMMONS_CLI_URLS = (origin + '/primary', origin + '/alternate')
        self.builder.XSDC_COMMONS_CLI_SHA256 = hashlib.sha256(PAYLOAD).hexdigest()

    def close_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.assertFalse(self.thread.is_alive())

    def reply(self, path, *responses):
        self.server.responses[path] = list(responses)

    def assert_cache(self, expected):
        if expected is None:
            self.assertFalse(self.jar.exists())
        else:
            self.assertEqual(self.jar.read_bytes(), expected)
        self.assertEqual(self.marker.read_bytes(), b'unchanged compiled tool')
        self.assertEqual(set(self.jar_dir.iterdir()),
                         {self.classes} | ({self.jar} if expected is not None else set()))

    def test_missing_jar_with_existing_classes_is_downloaded(self):
        self.reply('/primary', (200, PAYLOAD, None))
        command = self.builder.xsdc()
        self.assertEqual(command[-1], 'com.android.xsdc.Main')
        self.assertIn(str(self.jar), command[-2].split(BUILD.os.pathsep))
        self.assertEqual(self.server.requests, ['/primary'])
        self.assert_cache(PAYLOAD)

    def test_valid_cache_is_reused_without_network_or_replacement(self):
        self.jar.write_bytes(PAYLOAD)
        before = self.jar.stat().st_mtime_ns
        self.builder.xsdc()
        self.assertEqual(self.server.requests, [])
        self.assertEqual(self.jar.stat().st_mtime_ns, before)
        self.assert_cache(PAYLOAD)

    def test_corrupt_cache_is_replaced_even_with_existing_classes(self):
        self.jar.write_bytes(b'incomplete earlier download')
        self.reply('/primary', (200, PAYLOAD, None))
        self.builder.xsdc()
        self.assertEqual(self.server.requests, ['/primary'])
        self.assert_cache(PAYLOAD)

    def test_http_404_is_retried_before_success(self):
        self.reply('/primary', (404, b'missing', None), (200, PAYLOAD, None))
        self.builder.xsdc()
        self.assertEqual(self.server.requests, ['/primary', '/primary'])
        self.assert_cache(PAYLOAD)

    def test_http_503_is_retried_before_success(self):
        self.reply('/primary', (503, b'unavailable', None), (200, PAYLOAD, None))
        self.builder.xsdc()
        self.assertEqual(self.server.requests, ['/primary', '/primary'])
        self.assert_cache(PAYLOAD)

    def test_exhausted_primary_uses_the_alternate(self):
        self.reply('/alternate', (200, PAYLOAD, None))
        self.builder.xsdc()
        self.assertEqual(self.server.requests, ['/primary', '/primary', '/alternate'])
        self.assert_cache(PAYLOAD)

    def test_central_rate_limits_use_an_independent_verified_mirror(self):
        self.builder.XSDC_COMMONS_CLI_URLS += (self.builder.XSDC_COMMONS_CLI_URLS[0].replace('/primary', '/mirror'),)
        for path in ('/primary', '/alternate'):
            self.reply(path, (429, b'rate limited', None))
        self.reply('/mirror', (200, PAYLOAD, None))
        self.builder.xsdc()
        self.assertEqual(self.server.requests,
                         ['/primary', '/primary', '/alternate', '/alternate', '/mirror'])
        self.assert_cache(PAYLOAD)

    def test_rate_limited_sources_cannot_install_wrong_mirror_bytes(self):
        self.builder.XSDC_COMMONS_CLI_URLS += (self.builder.XSDC_COMMONS_CLI_URLS[0].replace('/primary', '/mirror'),)
        for path in ('/primary', '/alternate'):
            self.reply(path, (429, b'rate limited', None))
        self.reply('/mirror', (200, b'unverified mirror content', None))
        with self.assertRaisesRegex(RuntimeError, 'SHA-256 mismatch'):
            self.builder.xsdc()
        self.assert_cache(None)

    def test_all_404_responses_fail_after_bounded_attempts(self):
        with self.assertRaisesRegex(RuntimeError, 'cannot acquire verified Commons CLI 1.2'):
            self.builder.xsdc()
        self.assertEqual(self.server.requests,
                         ['/primary', '/primary', '/alternate', '/alternate'])
        self.assert_cache(None)

    def test_failed_download_never_reuses_a_corrupt_cache(self):
        self.jar.write_bytes(b'unverified existing cache')
        with self.assertRaises(RuntimeError):
            self.builder.xsdc()
        self.assert_cache(b'unverified existing cache')

    def test_truncated_transfers_are_not_published(self):
        for path in ('/primary', '/alternate'):
            self.reply(path, (200, PAYLOAD[:10], len(PAYLOAD)))
        with self.assertRaises(RuntimeError):
            self.builder.xsdc()
        self.assertEqual(self.server.requests,
                         ['/primary', '/primary', '/alternate', '/alternate'])
        self.assert_cache(None)

    def test_wrong_hash_is_rejected_and_temp_files_are_removed(self):
        self.jar.write_bytes(b'unverified existing cache')
        for path in ('/primary', '/alternate'):
            self.reply(path, (200, b'complete but incorrect bytes', None))
        with self.assertRaisesRegex(RuntimeError, 'SHA-256 mismatch'):
            self.builder.xsdc()
        self.assertEqual(self.server.requests, ['/primary', '/alternate'])
        self.assert_cache(b'unverified existing cache')

    def test_failed_atomic_publication_does_not_install_the_jar(self):
        self.reply('/primary', (200, PAYLOAD, None))

        def denied(source, destination):
            self.assertEqual(Path(source).read_bytes(), PAYLOAD)
            self.assertEqual(Path(source).parent.parent, self.jar_dir)
            self.assertEqual(Path(destination), self.jar)
            raise PermissionError('fixture publication denied')

        with mock.patch.object(BUILD.os, 'replace', side_effect=denied):
            with self.assertRaisesRegex(RuntimeError, 'publication failed'):
                self.builder.xsdc()
        self.assertEqual(self.server.requests, ['/primary'])
        self.assert_cache(None)

    def test_process_timeout_is_bounded_and_cleans_each_attempt(self):
        attempts = []

        def expired(command, **kwargs):
            self.assertEqual(kwargs['timeout'], 45)
            self.assertEqual(command[command.index('--max-time') + 1], '20')
            self.assertEqual(command[command.index('--connect-timeout') + 1], '10')
            self.assertEqual(command[command.index('--retry') + 1], '1')
            temporary = Path(command[command.index('-o') + 1])
            temporary.write_bytes(PAYLOAD[:10])
            attempts.append(temporary)
            raise subprocess.TimeoutExpired(command, kwargs['timeout'])

        with mock.patch.object(BUILD, 'run', side_effect=expired):
            with self.assertRaises(RuntimeError):
                self.builder.xsdc()
        self.assertEqual(len(attempts), 2)
        self.assertNotEqual(attempts[0].parent, attempts[1].parent)
        self.assertTrue(all(not path.parent.exists() for path in attempts))
        self.assertEqual(self.server.requests, [])
        self.assert_cache(None)


if __name__ == '__main__':
    unittest.main()
