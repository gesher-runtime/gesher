"""Pure-Python controls for reporting a worker failure without masking it."""
import importlib.util
from pathlib import Path
import queue
import unittest

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


if __name__ == '__main__':
    unittest.main()
