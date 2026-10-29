"""Pure-Python controls for reporting a worker failure without masking it."""
import importlib.util
import io
from pathlib import Path
import queue
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('command_oracle', Path(__file__).with_name('command_oracle.py'))
oracle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oracle)


class Stream:
    def close(self):
        raise OSError(22, 'injected close failure')


class Process:
    pid = 1234
    returncode = 94
    stdin = Stream()
    stdout = Stream()

    def poll(self):
        return self.returncode

    def wait(self, timeout):
        assert timeout == 5
        return self.returncode


class Reader:
    ident = 1
    def join(self, timeout):
        assert timeout == 2

    def is_alive(self):
        return False


class CleanupControls(unittest.TestCase):
    def client(self):
        client = oracle.Client.__new__(oracle.Client)
        client.process = Process()
        client.expected_exit = 0
        client.reader = Reader()
        client.lines = queue.Queue()
        return client

    def test_eof_collects_owned_exit(self):
        client = self.client()
        client.lines.put(None)
        with self.assertRaisesRegex(AssertionError, r'worker EOF.*94'):
            client.receive()

    def test_both_close_errors_and_native_exit_are_preserved(self):
        with self.assertRaises(AssertionError) as caught:
            self.client().cleanup()
        for required in ('owned stdout close', 'owned stdin close', 'worker exit 94'):
            self.assertIn(required, str(caught.exception))

    def test_replay_error_still_checks_file_close_and_native_exit(self):
        class Capture:
            def seek(self, offset):
                assert offset == 0

            def read(self, bound):
                assert bound == 1048577
                return b'actual native evidence\n'

            def close(self):
                raise OSError(5, 'injected evidence close failure')

        client = self.client()
        client.stderr_capture = Capture()
        with patch.object(oracle.sys.stderr, 'write', side_effect=OSError(5, 'injected replay failure')):
            with self.assertRaises(AssertionError) as caught:
                client.cleanup()
        for required in ('owned stderr evidence replay', 'owned stderr file close', 'worker exit 94'):
            self.assertIn(required, str(caught.exception))

    def test_evidence_cap_rejects_and_still_closes_file(self):
        class Capture:
            closed = False

            def seek(self, offset):
                pass

            def read(self, bound):
                return b'x' * bound

            def close(self):
                self.closed = True

        client = self.client()
        client.stderr_capture = Capture()
        with self.assertRaisesRegex(AssertionError, 'exceeded 1MiB'):
            client.cleanup()
        self.assertTrue(client.stderr_capture.closed)

    def partial_reader_start(self, confirmed):
        class StartingProcess:
            pid = 5678
            returncode = None
            stdin = io.StringIO()
            stdout = io.StringIO()
            waited = False

            def poll(self):
                return self.returncode

            def kill(self):
                if confirmed:
                    self.returncode = 91

            def wait(self, timeout):
                assert timeout == 5
                self.waited = True
                if self.returncode is None:
                    raise oracle.subprocess.TimeoutExpired('injected owned child', timeout)
                return self.returncode

        child, capture = StartingProcess(), io.BytesIO()
        with patch.object(oracle.os, 'name', 'nt'), patch.object(oracle.tempfile, 'TemporaryFile', return_value=capture), \
                patch.object(oracle.subprocess, 'Popen', return_value=child), \
                patch.object(oracle.threading.Thread, 'start', side_effect=RuntimeError('injected reader start failure')):
            with self.assertRaises(AssertionError) as caught:
                oracle.Client('unused-no-execution', [])
        self.assertTrue(child.waited)
        self.assertIsInstance(caught.exception.__context__, RuntimeError)
        if confirmed:
            self.assertTrue(child.stdin.closed and child.stdout.closed and capture.closed)
            self.assertIn('worker exit 91', str(caught.exception))
        else:
            self.assertIn('ownership retained', str(caught.exception))
            retained = oracle._QUARANTINED_CLIENTS.pop()
            self.assertIs(retained.process, child)
            self.assertIs(retained.stderr_capture, capture)
            self.assertFalse(child.stdin.closed or child.stdout.closed or capture.closed)
            # Fake child has no OS process. Close test-only objects after proving retention.
            child.stdin.close()
            child.stdout.close()
            capture.close()

    def test_reader_start_failure_retires_and_closes_every_capture(self):
        self.partial_reader_start(confirmed=True)

    def test_unconfirmed_partial_start_retains_exact_child_and_streams(self):
        self.partial_reader_start(confirmed=False)


if __name__ == '__main__':
    unittest.main()
