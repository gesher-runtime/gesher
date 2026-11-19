"""Exercise the JVM-tool downloader with real curl and isolated HTTP responses."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
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

JAVA = b'javapoet fixture'
GUAVA = b'guava fixture'


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
        self.server.responses = {'/primary/javapoet': [(200, JAVA, 0)],
                                 '/primary/guava': [(200, GUAVA, 0)],
                                 '/alternate/javapoet': [(200, JAVA, 0)],
                                 '/alternate/guava': [(200, GUAVA, 0)]}
        thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': 0.01})
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(self.server.shutdown)
        origin = f'http://127.0.0.1:{self.server.server_port}'
        self.builder.JVM_MAVEN_ORIGINS = (origin + '/primary', origin + '/alternate')
        self.builder.JVM_MAVEN_ARTIFACTS = {
            'javapoet': ('javapoet', hashlib.sha256(JAVA).hexdigest()),
            'guava': ('guava', hashlib.sha256(GUAVA).hexdigest()),
        }

    def test_rate_limit_recovers_and_successes_are_cached(self):
        self.server.responses['/primary/javapoet'].insert(0, (429, b'limited', 0))
        self.builder.jvm_tools()
        self.builder.jvm_tools()
        self.assertEqual(self.server.requests, ['/primary/javapoet'] * 2 + ['/primary/guava'])
        self.assertEqual((self.cache / 'javapoet.jar').read_bytes(), JAVA)
        self.assertEqual((self.cache / 'guava.jar').read_bytes(), GUAVA)

    def test_primary_404_uses_alternate_without_retrying_primary(self):
        self.server.responses['/primary/javapoet'] = [(404, b'missing', 0)]
        self.builder.jvm_tools()
        self.assertEqual(self.server.requests,
                         ['/primary/javapoet', '/alternate/javapoet', '/primary/guava'])
        self.assertEqual((self.cache / 'javapoet.jar').read_bytes(), JAVA)

    def test_permanent_http_error_does_not_retry_or_publish(self):
        self.builder.JVM_MAVEN_ORIGINS = self.builder.JVM_MAVEN_ORIGINS[:1]
        self.server.responses['/primary/javapoet'] = [(404, b'missing', 0)]
        with self.assertRaisesRegex(RuntimeError, '404'):
            self.builder.jvm_tools()
        self.assertEqual(self.server.requests, ['/primary/javapoet'])
        self.assertEqual({item.name for item in self.cache.iterdir()}, {'kotlinc'})

    def test_both_404s_fail_without_publishing(self):
        self.server.responses['/primary/javapoet'] = [(404, b'missing', 0)]
        self.server.responses['/alternate/javapoet'] = [(404, b'missing', 0)]
        with self.assertRaisesRegex(RuntimeError, '404'):
            self.builder.jvm_tools()
        self.assertEqual(self.server.requests, ['/primary/javapoet', '/alternate/javapoet'])
        self.assertEqual({item.name for item in self.cache.iterdir()}, {'kotlinc'})

    def test_interrupted_download_does_not_poison_cache(self):
        self.server.responses['/primary/javapoet'] = [(200, b'partial', 20)]
        self.server.responses['/alternate/javapoet'] = [(404, b'missing', 0)]
        with self.assertRaises(RuntimeError):
            self.builder.jvm_tools()
        self.assertEqual({item.name for item in self.cache.iterdir()}, {'kotlinc'})
        self.server.responses['/primary/javapoet'] = [(200, JAVA, 0)]
        self.builder.jvm_tools()
        self.assertEqual((self.cache / 'javapoet.jar').read_bytes(), JAVA)

    def test_wrong_bytes_are_rejected_from_cache_and_both_origins(self):
        jar = self.cache / 'javapoet.jar'
        jar.write_bytes(b'stale cache')
        self.server.responses['/primary/javapoet'] = [(200, b'wrong artifact', 0)]
        self.server.responses['/alternate/javapoet'] = [(200, b'wrong alternate', 0)]
        with self.assertRaisesRegex(RuntimeError, 'SHA-256 mismatch'):
            self.builder.jvm_tools()
        self.assertEqual(jar.read_bytes(), b'stale cache')
        self.assertEqual({item.name for item in self.cache.iterdir()}, {'kotlinc', jar.name})
        self.server.responses['/alternate/javapoet'] = [(200, JAVA, 0)]
        self.builder.jvm_tools()
        self.assertEqual(jar.read_bytes(), JAVA)

    def test_continuous_rate_limit_is_bounded(self):
        self.server.responses['/primary/javapoet'] = [(429, b'limited', 0)]
        self.server.responses['/alternate/javapoet'] = [(404, b'missing', 0)]
        with self.assertRaisesRegex(RuntimeError, '429'):
            self.builder.jvm_tools()
        self.assertEqual(self.server.requests, ['/primary/javapoet'] * 4 + ['/alternate/javapoet'])
        self.assertEqual({item.name for item in self.cache.iterdir()}, {'kotlinc'})


if __name__ == '__main__':
    unittest.main()
