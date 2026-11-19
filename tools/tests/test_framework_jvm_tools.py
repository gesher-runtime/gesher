"""Exercise the JVM-tool downloader with real curl and isolated HTTP responses."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest


SPEC = importlib.util.spec_from_file_location(
    'framework_jvm_tools_builder', Path(__file__).parents[1] / 'framework' / 'build.py')
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        responses = self.server.responses[self.path]
        status, data, missing = responses.pop(0) if len(responses) > 1 else responses[0]
        self.send_response(status)
        self.send_header('Content-Length', str(len(data) + missing))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *_):
        pass


class JvmToolsDownloadTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.cache = self.root / 'out' / 'jvm-tools'
        (self.cache / 'kotlinc').mkdir(parents=True)
        self.builder = BUILD.Builder(SimpleNamespace(
            aosp=str(self.root / 'aosp'), out=str(self.root / 'out')))
        self.server = self.enterContext(ThreadingHTTPServer(('127.0.0.1', 0), Handler))
        self.server.requests = []
        self.server.responses = {'/javapoet': [(200, b'javapoet fixture', 0)],
                                 '/guava': [(200, b'guava fixture', 0)]}
        thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': 0.01})
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(self.server.shutdown)
        origin = f'http://127.0.0.1:{self.server.server_port}'
        self.builder.JAVAPOET = origin + '/javapoet'
        self.builder.GUAVA = origin + '/guava'

    def test_rate_limit_recovers_and_successes_are_cached(self):
        self.server.responses['/javapoet'].insert(0, (429, b'limited', 0))
        self.builder.jvm_tools()
        self.builder.jvm_tools()
        self.assertEqual(self.server.requests, ['/javapoet', '/javapoet', '/guava'])
        self.assertEqual((self.cache / 'javapoet.jar').read_bytes(), b'javapoet fixture')
        self.assertEqual((self.cache / 'guava.jar').read_bytes(), b'guava fixture')

    def test_permanent_http_error_does_not_retry_or_publish(self):
        self.server.responses['/javapoet'] = [(404, b'missing', 0)]
        with self.assertRaisesRegex(RuntimeError, '404'):
            self.builder.jvm_tools()
        self.assertEqual(self.server.requests, ['/javapoet'])
        self.assertEqual({item.name for item in self.cache.iterdir()}, {'kotlinc'})

    def test_interrupted_download_does_not_poison_cache(self):
        self.server.responses['/javapoet'] = [(200, b'partial', 20)]
        with self.assertRaises(RuntimeError):
            self.builder.jvm_tools()
        self.assertEqual({item.name for item in self.cache.iterdir()}, {'kotlinc'})
        self.server.responses['/javapoet'] = [(200, b'complete', 0)]
        self.builder.jvm_tools()
        self.assertEqual((self.cache / 'javapoet.jar').read_bytes(), b'complete')

    def test_continuous_rate_limit_is_bounded(self):
        self.server.responses['/javapoet'] = [(429, b'limited', 0)]
        with self.assertRaisesRegex(RuntimeError, '429'):
            self.builder.jvm_tools()
        self.assertEqual(self.server.requests, ['/javapoet'] * 4)
        self.assertEqual({item.name for item in self.cache.iterdir()}, {'kotlinc'})


if __name__ == '__main__':
    unittest.main()
