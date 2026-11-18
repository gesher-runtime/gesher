"""Exercise the production shell downloader against isolated HTTP failures."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

PAYLOAD = b'isolated build-tool transport fixture\n'


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


class BuildToolDownload(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.destination = self.root / 'r8.jar'
        self.bash = shutil.which('bash')
        if sys.platform == 'win32':
            self.bash = str(Path(shutil.which('git')).resolve().parents[1] / 'bin' / 'bash.exe')
        source = Path(__file__).with_name('build.sh').read_text(encoding='utf-8')
        helper = source.split('download_build_tool() (\n', 1)[1].split('\n)\n', 1)[0]
        self.script = self.root / 'download.sh'
        self.script.write_text('set -euo pipefail\ndownload_build_tool() (\n' + helper +
                               '\n)\ndownload_build_tool "$1" "$2"\n', encoding='utf-8')
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.requests = 0
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(poll_interval=0.01), daemon=True)
        self.thread.start()
        self.addCleanup(self.close)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def download(self, replies):
        self.server.replies = list(replies)
        return subprocess.run([self.bash, self.script.as_posix(),
            self.destination.as_posix(), f'http://127.0.0.1:{self.server.server_port}/tool'],
            capture_output=True, text=True, timeout=30)

    def test_partial_transfer_is_retried_and_only_complete_bytes_are_published(self):
        result = self.download([(200, PAYLOAD[:5], len(PAYLOAD)), (200, PAYLOAD, None)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.server.requests, 2)
        self.assertEqual(self.destination.read_bytes(), PAYLOAD)
        self.assertEqual(list(self.root.glob('*.part.*')), [])

    def test_transient_server_error_is_retried(self):
        result = self.download([(503, b'unavailable', None), (200, PAYLOAD, None)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.server.requests, 2)
        self.assertEqual(self.destination.read_bytes(), PAYLOAD)

    def test_exhausted_transfers_do_not_publish_a_partial_tool(self):
        result = self.download([(200, PAYLOAD[:5], len(PAYLOAD))])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.server.requests, 3)
        self.assertFalse(self.destination.exists())
        self.assertEqual(list(self.root.glob('*.part.*')), [])

    def test_download_failure_preserves_an_existing_local_tool(self):
        self.destination.write_bytes(b'existing tool')
        result = self.download([(503, b'unavailable', None)])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.server.requests, 3)
        self.assertEqual(self.destination.read_bytes(), b'existing tool')
        self.assertEqual(list(self.root.glob('*.part.*')), [])


if __name__ == '__main__':
    unittest.main()
