"""The report gate must fail on missing or unsuccessful selected evidence."""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'run_api37_report_tests', Path(__file__).with_name('run_api37_report_tests.py'))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class ReportTestGateTests(unittest.TestCase):
    def result_for(self, action):
        class Fixture(unittest.TestCase):
            def runTest(self):
                action(self)
        return gate.run_selected([Fixture()], stream=io.StringIO())

    def test_passed_test_succeeds(self):
        success, result = self.result_for(lambda case: case.assertEqual(1 + 1, 2))
        self.assertTrue(success)
        self.assertEqual((result['selected'], result['executed'], result['skipped']), (1, 1, 0))

    def test_assertion_failure_fails(self):
        success, result = self.result_for(lambda case: case.fail('ordinary assertion control'))
        self.assertFalse(success)
        self.assertEqual(result['failures'], 1)

    def test_test_error_fails(self):
        def error(case):
            raise RuntimeError('ordinary execution-error control')
        success, result = self.result_for(error)
        self.assertFalse(success)
        self.assertEqual(result['errors'], 1)

    def test_missing_tool_skip_fails(self):
        success, result = self.result_for(lambda case: case.skipTest('missing prerequisite control'))
        self.assertFalse(success)
        self.assertEqual(result['skipped'], 1)

    def test_empty_selection_fails(self):
        success, result = gate.run_selected([], stream=io.StringIO())
        self.assertFalse(success)
        self.assertEqual(result['executed'], 0)

    def test_class_setup_skip_cannot_succeed_with_unexecuted_tests(self):
        class Fixture(unittest.TestCase):
            @classmethod
            def setUpClass(cls):
                raise unittest.SkipTest('missing class prerequisite control')
            def runTest(self):
                self.fail('unreachable')
        success, result = gate.run_selected([Fixture()], stream=io.StringIO())
        self.assertFalse(success)
        self.assertEqual((result['selected'], result['executed'], result['skipped']), (1, 0, 1))

    def test_expected_failure_is_not_successful_evidence(self):
        class Fixture(unittest.TestCase):
            @unittest.expectedFailure
            def runTest(self):
                self.fail('expected failure control')
        success, result = gate.run_selected([Fixture()], stream=io.StringIO())
        self.assertFalse(success)
        self.assertEqual(result['expected_failures'], 1)

    def test_windows_exclusions_are_exact_and_linux_still_selects_them(self):
        linux, linux_excluded = gate.select_tests('linux')
        windows, windows_excluded = gate.select_tests('windows')
        linux_ids, windows_ids = {t.id() for t in linux}, {t.id() for t in windows}
        self.assertEqual(linux_excluded, [])
        self.assertEqual(set(windows_excluded), gate.LINUX_ELF_TESTS)
        self.assertEqual(linux_ids - windows_ids, gate.LINUX_ELF_TESTS)
        self.assertEqual(windows_ids - linux_ids, set())
        for module in gate.MODULES:
            self.assertTrue(any(identity.startswith(module + '.') for identity in windows_ids), module)

    def test_missing_declared_module_fails(self):
        with patch.object(gate, 'MODULES', ('absent_api37_report_module',)):
            with self.assertRaisesRegex(ValueError, 'Cannot load complete report module'):
                gate.select_tests('linux')

    def test_wrong_platform_is_rejected_before_selection(self):
        with patch.object(gate.sys, 'platform', 'linux'), patch.object(gate, 'select_tests') as selected:
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    gate.main(['--profile', 'windows'])
        self.assertEqual(error.exception.code, 2)
        selected.assert_not_called()

    def test_cli_status_preserves_selected_test_failure(self):
        class Fixture(unittest.TestCase):
            def runTest(self):
                self.fail('ordinary CLI failure propagation control')
        with patch.object(gate.sys, 'platform', 'linux'):
            with patch.object(gate, 'select_tests', return_value=([Fixture()], [])):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(gate.main(['--profile', 'linux']), 1)

    def test_cli_status_accepts_selected_success(self):
        class Fixture(unittest.TestCase):
            def runTest(self):
                self.assertEqual(2 + 2, 4)
        with patch.object(gate.sys, 'platform', 'linux'):
            with patch.object(gate, 'select_tests', return_value=([Fixture()], [])):
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(gate.main(['--profile', 'linux']), 0)


if __name__ == '__main__':
    unittest.main()
