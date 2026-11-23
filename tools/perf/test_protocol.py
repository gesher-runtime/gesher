"""Live mailbox I/O, strict failure behavior and actual Windows sharing controls."""
from __future__ import annotations

import errno
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

import protocol_io as protocol

ROOT = Path(__file__).resolve().parents[2]


def native_error(code):
    error = OSError(errno.EACCES, "controlled native access failure")
    error.winerror = code
    return error


class ProtocolTests(unittest.TestCase):
    def test_only_classified_windows_errors_retry_and_success_is_retried_operation(self):
        for code in (5, 32):
            operation = Mock(side_effect=[native_error(code), {"fresh": True}])
            with self.subTest(code=code), patch.object(protocol, "WINDOWS", True), \
                    patch.object(protocol.time, "sleep") as sleep:
                self.assertEqual(protocol._retry(operation, time.monotonic() + 10), {"fresh": True})
                self.assertEqual(operation.call_count, 2)
                sleep.assert_called_once_with(protocol.RETRY_SECONDS)

    def test_persistent_native_denial_is_bounded_and_preserves_error(self):
        for code in (5, 32):
            error = native_error(code)
            operation = Mock(side_effect=error)
            with self.subTest(code=code), patch.object(protocol, "WINDOWS", True), \
                    patch.object(protocol.time, "sleep") as sleep, self.assertRaises(OSError) as failed:
                protocol._retry(operation, time.monotonic() + 10)
            self.assertIs(failed.exception, error)
            self.assertEqual(operation.call_count, 5)
            self.assertEqual([call.args[0] for call in sleep.call_args_list],
                             [protocol.RETRY_SECONDS * (2 ** retry) for retry in range(4)])

    def test_generic_permission_other_native_errors_and_cancellation_are_not_retried(self):
        for windows, error in [(True, PermissionError(errno.EACCES, "unclassified")),
                               (True, native_error(3)), (True, native_error(33)),
                               (False, native_error(5)), (True, KeyboardInterrupt())]:
            operation = Mock(side_effect=error)
            with self.subTest(windows=windows, error=error), patch.object(protocol, "WINDOWS", windows), \
                    patch.object(protocol.time, "sleep") as sleep, self.assertRaises(type(error)):
                protocol._retry(operation, time.monotonic() + 10)
            operation.assert_called_once()
            sleep.assert_not_called()

    def test_deadline_is_not_refreshed_and_late_success_is_rejected(self):
        clock = [0.0]
        operation = Mock(side_effect=native_error(32))
        delays = []
        def sleep(delay):
            delays.append(delay)
            clock[0] += delay
        with patch.object(protocol, "WINDOWS", True), patch.object(protocol.time, "monotonic", side_effect=lambda: clock[0]), \
                patch.object(protocol.time, "sleep", side_effect=sleep), self.assertRaises(TimeoutError):
            protocol._retry(operation, .015)
        self.assertEqual(operation.call_count, 2)
        self.assertAlmostEqual(sum(delays), .015)
        operation = Mock(side_effect=lambda: clock.__setitem__(0, 2))
        clock[0] = 0
        with patch.object(protocol.time, "monotonic", side_effect=lambda: clock[0]), self.assertRaises(TimeoutError):
            protocol._retry(operation, 1)
        operation.assert_called_once()
        with self.assertRaises(TimeoutError): protocol._retry(Mock(side_effect=AssertionError("expired I/O")), 0)

    def test_actual_json_roundtrip_and_semantic_errors_do_not_retry(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "phase.json"
            protocol.write_json(path, {"invocation_id": "a" * 32, "phase": "start"}, deadline=time.monotonic() + 10)
            self.assertEqual(protocol.read_json(path, deadline=time.monotonic() + 10)["phase"], "start")
            for value in [b'{invalid', b'{"phase":"start","phase":"stop"}', b'{"value":NaN}', b'\xff',
                          b'x' * (protocol.MAX_BYTES + 1)]:
                path.write_bytes(value)
                with self.subTest(prefix=value[:30]), patch.object(protocol, "_read_snapshot", wraps=protocol._read_snapshot) as read, \
                        patch.object(protocol.time, "sleep") as sleep, self.assertRaises(ValueError):
                    protocol.read_json(path, deadline=time.monotonic() + 10)
                read.assert_called_once()
                sleep.assert_not_called()

    def test_failed_publication_keeps_old_message_and_unpublished_complete_temporary(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "ack.json"
            protocol.write_json(path, {"phase": "start"}, deadline=time.monotonic() + 10)
            with patch.object(protocol, "WINDOWS", True), patch.object(protocol, "_publish", side_effect=native_error(5)) as publish, \
                    patch.object(protocol.time, "sleep"), self.assertRaises(OSError):
                protocol.write_json(path, {"phase": "stop"}, deadline=time.monotonic() + 10)
            self.assertEqual(publish.call_count, 5)
            self.assertEqual(json.loads(path.read_text())["phase"], "start")
            self.assertEqual(json.loads(path.with_name("ack.json.tmp").read_text())["phase"], "stop")
            with self.assertRaises(ValueError):
                protocol.write_json(path, {"invalid": float("nan")}, deadline=time.monotonic() + 10)
            self.assertEqual(json.loads(path.read_text())["phase"], "start")


@unittest.skipUnless(os.name == "nt", "Actual Windows handle sharing")
class WindowsProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gesher-protocol-")
        self.path = Path(self.temporary.name) / "phase.json"
        protocol.write_json(self.path, {"phase": "start"}, deadline=time.monotonic() + 10)

    def tearDown(self):
        self.temporary.cleanup()

    def test_read_coexists_with_live_delete_handle_that_rejects_ordinary_python_read(self):
        c, kernel = protocol._kernel()
        handle = kernel.CreateFileW(str(self.path), 0x10000, 7, None, 3, 0x80, None)
        self.assertNotEqual(handle, c.c_void_p(-1).value)
        try:
            with self.assertRaises(PermissionError): self.path.read_bytes()
            self.assertEqual(protocol.read_json(self.path, deadline=time.monotonic() + 10)["phase"], "start")
        finally:
            kernel.CloseHandle(handle)

    def test_replacement_retains_complete_old_snapshot_and_new_reader_sees_new_message(self):
        with protocol._open_snapshot(self.path) as old, patch.object(protocol.time, "sleep", side_effect=AssertionError("unexpected retry")):
            self.assertFalse(os.get_inheritable(old.fileno()))
            protocol.write_json(self.path, {"phase": "stop"}, deadline=time.monotonic() + 10)
            self.assertEqual(json.loads(old.read())["phase"], "start")
            self.assertEqual(protocol.read_json(self.path, deadline=time.monotonic() + 10)["phase"], "stop")

    def test_actual_blocked_replacement_retries_after_holder_release(self):
        holder = self.path.open("rb")  # Ordinary reader does not share DELETE.
        calls = []
        original = protocol._publish
        def publish(*args):
            try: return original(*args)
            except OSError as error:
                calls.append(error.winerror)
                raise
        try:
            with patch.object(protocol, "_publish", side_effect=publish), \
                    patch.object(protocol.time, "sleep", side_effect=lambda _: holder.close()) as sleep:
                protocol.write_json(self.path, {"phase": "stop"}, deadline=time.monotonic() + 10)
            self.assertEqual(len(calls), 1)
            self.assertIn(calls[0], (5, 32))
            sleep.assert_called_once()
            self.assertEqual(protocol.read_json(self.path, deadline=time.monotonic() + 10)["phase"], "stop")
        finally:
            holder.close()

    def test_actual_persistent_holder_fails_without_overwriting_or_dropping_message(self):
        with self.path.open("rb"), patch.object(protocol.time, "sleep") as sleep, self.assertRaises(OSError) as failed:
            protocol.write_json(self.path, {"phase": "stop"}, deadline=time.monotonic() + 10)
        self.assertIn(failed.exception.winerror, (5, 32))
        self.assertEqual(sleep.call_count, 4)
        self.assertEqual(protocol.read_json(self.path, deadline=time.monotonic() + 10)["phase"], "start")
        self.assertEqual(json.loads(self.path.with_name("phase.json.tmp").read_text())["phase"], "stop")

    def test_native_read_conflict_recovers_after_release_but_persistent_conflict_fails(self):
        c, kernel = protocol._kernel()
        for release in (True, False):
            with self.subTest(release=release):
                handle = kernel.CreateFileW(str(self.path), 0x80000000, 0, None, 3, 0x80, None)
                self.assertNotEqual(handle, c.c_void_p(-1).value)
                def release_handle(_):
                    nonlocal handle
                    if release and handle is not None:
                        kernel.CloseHandle(handle); handle = None
                try:
                    with patch.object(protocol.time, "sleep", side_effect=release_handle) as sleep:
                        if release:
                            self.assertEqual(protocol.read_json(self.path, deadline=time.monotonic() + 10)["phase"], "start")
                            self.assertEqual(sleep.call_count, 1)
                        else:
                            with self.assertRaises(OSError) as failed:
                                protocol.read_json(self.path, deadline=time.monotonic() + 10)
                            self.assertEqual(failed.exception.winerror, 32)
                            self.assertEqual(sleep.call_count, 4)
                finally:
                    if handle is not None: kernel.CloseHandle(handle)

    def test_actual_python_and_powershell_share_complete_mailboxes(self):
        script = ROOT / "tests/e2e/performance-protocol.test.ps1"
        data = self.path.parent / "new-message.json"
        data.write_text('{"phase":"stop"}')
        for shell in ("pwsh", "powershell.exe"):
            with self.subTest(shell=shell):
                executable = shutil.which(shell)
                self.assertIsNotNone(executable, f"Required Windows protocol consumer missing: {shell}")
                protocol.write_json(self.path, {"phase": "start"}, deadline=time.monotonic() + 10)
                with protocol._open_snapshot(self.path) as held:
                    written = subprocess.run([executable, "-NoProfile", "-File", str(script), "-Action", "Write", "-Path", str(self.path),
                                              "-ValueFile", str(data)], capture_output=True, text=True, timeout=15)
                    self.assertEqual(written.returncode, 0, (written.stdout + written.stderr)[-8192:])
                    self.assertEqual(json.loads(held.read())["phase"], "start")
                c, kernel = protocol._kernel()
                handle = kernel.CreateFileW(str(self.path), 0x10000, 7, None, 3, 0x80, None)
                self.assertNotEqual(handle, c.c_void_p(-1).value)
                try:
                    read = subprocess.run([executable, "-NoProfile", "-File", str(script), "-Action", "Read", "-Path", str(self.path)],
                                          capture_output=True, text=True, timeout=15)
                    self.assertEqual(read.returncode, 0, (read.stdout + read.stderr)[-8192:])
                    self.assertEqual(json.loads(read.stdout)["phase"], "stop")
                finally:
                    kernel.CloseHandle(handle)


if __name__ == "__main__":
    unittest.main()
