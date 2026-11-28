"""Exercise both real fixture fetchers with isolated HTTP transport responses."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock
import urllib.error
import urllib.request

NAME = 'sqlite-jdbc-3.46.1.3.jar'
PAYLOAD = b'isolated transport fixture; never loaded as a JDBC driver\n'
DIGEST = hashlib.sha256(PAYLOAD).hexdigest()
MODULES = []
for name in ('test_download_manager_contracts', 'test_media_provider_contracts'):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).parents[1] / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    MODULES.append(module)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        status, body = self.server.replies.pop(0)
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


class JdbcFixtureDownload(unittest.TestCase):
    def setUp(self):
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(poll_interval=0.01), daemon=True)
        self.thread.start()
        self.addCleanup(self.close)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def exercise(self, replies, *, failure=None, calls=1, cached=False, network_error=False):
        # Preserve the real pinned URLs; only the digest and HTTP responses are fixtures.
        real_urlopen = urllib.request.urlopen
        for module in MODULES:
            with self.subTest(fetcher=module.__name__), tempfile.TemporaryDirectory() as temporary:
                cache = Path(temporary)
                if cached:
                    (cache / NAME).write_bytes(PAYLOAD)
                self.server.replies = list(replies)
                requested = []

                def transport(url, timeout):
                    requested.append(url)
                    self.assertEqual(timeout, 120)
                    if network_error and len(requested) == 1:
                        raise urllib.error.URLError(ConnectionResetError('isolated reset'))
                    return real_urlopen(f'http://127.0.0.1:{self.server.server_port}/fixture', timeout=timeout)

                primary = module.INPUTS[NAME][0]
                with mock.patch.dict(module.INPUTS, {NAME: (primary, DIGEST)}), \
                        mock.patch.object(module.urllib.request, 'urlopen', side_effect=transport):
                    if failure:
                        with self.assertRaises(failure):
                            module.fetch(cache, NAME)
                        self.assertEqual(list(cache.iterdir()), [], 'Failed fetch published cache bytes')
                    else:
                        target = module.fetch(cache, NAME)
                        self.assertEqual(target.read_bytes(), PAYLOAD)
                        self.assertEqual(list(cache.iterdir()), [target])
                self.assertEqual(len(requested), calls)
                if requested:
                    self.assertEqual(requested[0], primary)
                if len(requested) == 2:
                    self.assertEqual(requested[1], 'https://maven-central.storage-download.googleapis.com/'
                                     'maven2/org/xerial/sqlite-jdbc/3.46.1.3/sqlite-jdbc-3.46.1.3.jar')

    def test_primary_success(self):
        self.exercise([(200, PAYLOAD)])

    def test_transient_http_uses_verified_mirror(self):
        for status in (429, 500, 502, 503, 504):
            with self.subTest(status=status):
                self.exercise([(status, b'unavailable'), (200, PAYLOAD)], calls=2)

    def test_network_failure_uses_verified_mirror(self):
        self.exercise([(200, PAYLOAD)], calls=2, network_error=True)

    def test_corrupt_primary_does_not_use_mirror(self):
        self.exercise([(200, b'wrong content')], failure=RuntimeError)

    def test_corrupt_mirror_is_rejected(self):
        self.exercise([(429, b'unavailable'), (200, b'wrong content')], calls=2, failure=RuntimeError)

    def test_both_transports_fail_without_cache_publication(self):
        self.exercise([(429, b'unavailable'), (503, b'unavailable')], calls=2, failure=urllib.error.HTTPError)

    def test_permanent_failure_does_not_use_mirror(self):
        for status in (401, 403, 404):
            with self.subTest(status=status):
                self.exercise([(status, b'failure')], failure=urllib.error.HTTPError)

    def test_valid_cache_never_uses_network(self):
        self.exercise([], calls=0, cached=True)


if __name__ == '__main__':
    unittest.main()
