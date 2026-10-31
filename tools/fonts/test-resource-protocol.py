"""Pure state/receipt controls only. Starts zero processes, creates no handles."""
import importlib.util
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location("resource_protocol", Path(__file__).with_name("resource-protocol.py"))
p = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = p
spec.loader.exec_module(p)


def finish(s, role, pid, at, code=0):
    s.exited(role, pid, code, kernel_complete=True, now=at)
    s.dispose(role, pipes_closed=True, now=at)


def prepared(*, help=False, prewarm=True):
    s = p.Session(optional_help=help, prewarm=prewarm)
    s.copied_handle(0)
    s.start("imports", 50, 0)
    finish(s, "imports", 50, 0)
    s.start("child", 100, 1)
    if help:
        s.start("help", 200, 2)
        finish(s, "help", 200, 3)
    if prewarm:
        s.start("prewarm", 300, 4)
        finish(s, "prewarm", 300, 5)
    s.prepared(target_ready=True, stage_rechecked=True, symbols_prepared=True, now=6)
    return s


def held(**kwargs):
    s = prepared(**kwargs)
    s.held(100, 10, 11, both_native_pending=True, now=7)
    return s


class Protocol(unittest.TestCase):
    def test_complete_all_owned_roles(self):
        s = held(help=True)
        s.start("capture", 400, 8)
        finish(s, "capture", 400, 9)
        s.captured(raw_evidence_recorded=True, now=10)
        s.released(release_delivered=True, now=11)
        finish(s, "child", 100, 12)
        s.close_copied_handles(1, 13)
        s.complete(14)
        self.assertTrue(s.release_root())
        self.assertFalse(s.release_root())
        evidence = s.evidence()
        self.assertEqual(evidence["phase"], "COMPLETE")
        self.assertFalse(evidence["root_retained"])
        self.assertEqual(evidence["gallery"], ("NOT_DISABLED", "NOT_EXHAUSTIVE"))
        self.assertTrue(evidence["metadata_only"])
        self.assertFalse(evidence["hold_is_owner_proof"])
        self.assertEqual(set(evidence["qualification"].values()), {False})

    def test_help_omitted_is_not_prewarm(self):
        s = p.Session()
        self.assertEqual(s.roles["help"].state, p.ProcessState.OMITTED)
        self.assertEqual(s.roles["prewarm"].state, p.ProcessState.NOT_STARTED)
        s.start("child", 100, 0)
        with self.assertRaises(p.ProtocolError):
            s.prepared(target_ready=True, stage_rechecked=True, symbols_prepared=True, now=1)
        self.assertEqual(prepared(prewarm=False).roles["prewarm"].state, p.ProcessState.OMITTED)

    def test_no_role_replacement_reuse_or_untracked_process(self):
        for name, pid in (("child", 101), ("capture", 100), ("other", 500), ("help", 600)):
            s = held()
            with self.subTest(name=name, pid=pid), self.assertRaises(p.ProtocolError):
                s.start(name, pid, 8)
        s = held(help=True)
        with self.assertRaises(p.ProtocolError): s.start("help", 999, 8)

    def test_dword_type_bounds_and_distinct_tids(self):
        for value in (False, True, 0, -1, 0x100000000, "123", 1.5):
            with self.subTest(value=value), self.assertRaises(p.ProtocolError): p.dword(value)
        self.assertEqual(p.dword(0xFFFFFFFF), 0xFFFFFFFF)
        for ids in ((101, 10, 11), (100, 10, 10), (100, 0, 11)):
            s = prepared()
            with self.subTest(ids=ids), self.assertRaises(p.ProtocolError):
                s.held(*ids, both_native_pending=True, now=7)

    def test_help_does_not_authorize_prewarm_or_hold(self):
        s = p.Session(optional_help=True)
        s.start("help", 200, 0)
        with self.assertRaises(p.ProtocolError): s.start("prewarm", 300, 1)
        finish(s, "help", 200, 2)
        with self.assertRaises(p.ProtocolError): s.held(100, 10, 11, both_native_pending=True, now=3)
        s.start("child", 100, 4)
        s.start("prewarm", 300, 5)

    def test_actual_preparation_receipts_all_required(self):
        for field in ("target_ready", "stage_rechecked", "symbols_prepared"):
            s = p.Session(prewarm=False)
            s.start("child", 100, 0)
            args = dict(target_ready=True, stage_rechecked=True, symbols_prepared=True, now=1)
            args[field] = False
            with self.subTest(field=field), self.assertRaises(p.ProtocolError): s.prepared(**args)

    def test_pending_and_raw_are_not_owner_proof(self):
        s = prepared()
        with self.assertRaises(p.ProtocolError): s.held(100, 10, 11, both_native_pending=False, now=7)
        s.held(100, 10, 11, both_native_pending=True, now=8)
        s.start("capture", 400, 9)
        finish(s, "capture", 400, 10)
        with self.assertRaises(p.ProtocolError): s.captured(raw_evidence_recorded=False, now=11)
        s.captured(raw_evidence_recorded=True, now=12)
        self.assertTrue(all(value is False for value in s.evidence()["qualification"].values()))

    def test_true_kernel_same_identity_and_pipes(self):
        s = held()
        for pid, complete in ((101, True), (100, False)):
            with self.subTest(pid=pid, complete=complete), self.assertRaises(p.ProtocolError):
                s.exited("child", pid, 0, kernel_complete=complete, now=8)
        with self.assertRaises(p.ProtocolError): s.dispose("child", pipes_closed=True, now=9)
        s.exited("child", 100, 0, kernel_complete=True, now=10)
        with self.assertRaises(p.ProtocolError): s.dispose("child", pipes_closed=False, now=11)

    def test_monotonic_and_no_hold_budget_reset(self):
        s = held()
        deadline = s.hold_deadline
        with self.assertRaises(p.ProtocolError): s.start("capture", 400, 6)
        with self.assertRaises(p.ProtocolError): s.held(100, 10, 11, both_native_pending=True, now=8)
        self.assertEqual(s.hold_deadline, deadline)
        with self.assertRaises(p.ProtocolError): s.start("capture", 400, deadline)
        s.fail("expired", deadline + 1)
        self.assertEqual(s.hold_deadline, deadline)

    def test_setup_copy_and_phase_caps(self):
        s = p.Session()
        with self.assertRaises(p.ProtocolError): s.copied_handle(p.SETUP_MS + 1)
        s = p.Session()
        for _ in range(128): s.copied_handle(0)
        with self.assertRaises(p.ProtocolError): s.copied_handle(0)
        s = held()
        with self.assertRaises(p.ProtocolError): s.copied_handle(8)
        with self.assertRaises(p.ProtocolError): s.prepared(target_ready=True, stage_rechecked=True, symbols_prepared=True, now=9)

    def test_phase_cannot_skip_release_or_capture(self):
        for action in (lambda s: s.captured(raw_evidence_recorded=True, now=8),
                       lambda s: s.released(release_delivered=True, now=8),
                       lambda s: s.complete(8), lambda s: s.release_root()):
            s = held()
            with self.assertRaises(p.ProtocolError): action(s)

    def test_failed_or_live_roles_keep_root_and_copied_handles(self):
        for live in ("child", "help", "prewarm", "capture"):
            s = p.Session(optional_help=True, prewarm=True)
            s.copied_handle(0)
            s.start("child", 100, 1)
            if live != "child":
                if live == "capture":
                    finish(s, "child", 100, 2)
                    # Build capture state via public events in a separate session.
                    s = held(help=True); s.start("capture", 400, 8)
                else:
                    if live == "prewarm":
                        s.start("help", 250, 2)
                        finish(s, "help", 250, 3)
                    s.start(live, 200, 4)
                    finish(s, "child", 100, 5)
            s.fail("forced owned-process failure", 10)
            with self.subTest(live=live):
                with self.assertRaises(p.ProtocolError): s.close_copied_handles(1, 11)
                with self.assertRaises(p.ProtocolError): s.release_root()
                self.assertTrue(s.root_reserved)

    def test_late_cleanup_can_be_recorded_but_cannot_turn_failure_green(self):
        s = held()
        s.fail("timeout", p.SESSION_MS + 1)
        finish(s, "child", 100, p.SESSION_MS + 2)
        s.close_copied_handles(1, p.SESSION_MS + 3)
        self.assertTrue(s.release_root())
        self.assertEqual(s.phase, p.Phase.FAILED)
        with self.assertRaises(p.ProtocolError): s.complete(p.SESSION_MS + 4)

    def test_nonzero_role_result_never_completes_success(self):
        s = held()
        s.start("capture", 400, 8)
        finish(s, "capture", 400, 9, code=1)
        with self.assertRaises(p.ProtocolError): s.captured(raw_evidence_recorded=True, now=10)
        s.fail("capture exit 1", 11)
        finish(s, "child", 100, 12)
        s.close_copied_handles(1, 13)
        self.assertTrue(s.release_root())

    def test_failure_before_start_and_close_count(self):
        s = p.Session()
        s.copied_handle(0)
        s.fail("before any process", 1)
        with self.assertRaises(p.ProtocolError): s.release_root()
        for count in (-1, 2, True):
            with self.subTest(count=count), self.assertRaises(p.ProtocolError): s.close_copied_handles(count, 2)
        s.close_copied_handles(1, 3)
        self.assertTrue(s.release_root())
        with self.assertRaises(p.ProtocolError): s.start("child", 100, 4)

    def test_child_cannot_die_before_hold_capture_or_detach(self):
        s = prepared()
        finish(s, "child", 100, 7)
        with self.assertRaises(p.ProtocolError): s.held(100, 10, 11, both_native_pending=True, now=8)
        s = held()
        finish(s, "child", 100, 8)
        with self.assertRaises(p.ProtocolError): s.start("capture", 400, 9)
        s = held()
        s.start("capture", 400, 8)
        finish(s, "capture", 400, 9)
        finish(s, "child", 100, 10)
        with self.assertRaises(p.ProtocolError): s.captured(raw_evidence_recorded=True, now=11)
        s = held()
        s.start("capture", 400, 8)
        finish(s, "capture", 400, 9)
        s.captured(raw_evidence_recorded=True, now=10)
        finish(s, "child", 100, 11)
        with self.assertRaises(p.ProtocolError): s.released(release_delivered=True, now=12)

    def test_no_early_handle_close_or_replaced_failure(self):
        s = p.Session()
        s.copied_handle(0)
        with self.assertRaises(p.ProtocolError): s.close_copied_handles(1, 1)
        s.fail("first failure", 2)
        with self.assertRaises(p.ProtocolError): s.fail("replacement failure", 3)
        self.assertEqual(s.failure, "first failure")

    def test_native_quarantine_is_distinct_from_clr_root(self):
        for handler, writer in ((False, False), (True, False), (False, True)):
            self.assertEqual(p.NativeLifetime(handler, writer).disposition(), "QUARANTINE_STORAGE_EVENTS_AND_HANDLER_IN_LIVE_CHILD")
        self.assertEqual(p.NativeLifetime(True, True).disposition(), "NATIVE_STORAGE_MAY_RETIRE_AND_HANDLER_MAY_RESTORE")
        self.assertEqual(p.NativeLifetime(False, False, True).disposition(), "OS_RECLAIM_AFTER_EXACT_CHILD_TERMINATION")
        with self.assertRaises(p.ProtocolError): p.NativeLifetime(handler_complete=1).disposition()


if __name__ == "__main__":
    unittest.main()
