"""Historical Scandir evidence survives live-tree edits and rejects altered proof."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'scandir_report_verifier', ROOT / 'tools/test_scandir_report.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class ScandirReportTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.report = json.loads(verifier.REPORT.read_text(encoding='utf-8'))
        self.report_path = self.root / 'report.json'
        self.archive = self.root / self.report['archive']['path']
        self.archive.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / self.report['archive']['path'], self.archive)

    def verify(self, report=None):
        self.report_path.write_text(
            json.dumps(self.report if report is None else report), encoding='utf-8')
        with patch.object(verifier, 'ROOT', self.root), \
                patch.object(verifier, 'REPORT', self.report_path), \
                contextlib.redirect_stdout(io.StringIO()):
            verifier.main()

    def test_archive_verifies_without_live_source_or_git_history(self):
        self.verify()

    def test_unrelated_and_production_edits_do_not_relabel_historical_results(self):
        for name in ('.github/workflows/ci.yml', 'compat/bionic/shim_internal.h',
                     'tools/net-tcp-priority/test-selected-policy.py',
                     'compat/bionic/posix_bionic.cc'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('changed live source', encoding='utf-8')
        self.verify()

    def test_changed_archive_is_rejected_even_with_updated_report_digest(self):
        changed = self.archive.read_bytes() + b'changed'
        self.archive.write_bytes(changed)
        with self.assertRaisesRegex(AssertionError, 'archive digest'):
            self.verify()
        report = copy.deepcopy(self.report)
        report['archive']['sha256'] = verifier.digest(changed)
        report['archive']['bytes'] = len(changed)
        with self.assertRaisesRegex(AssertionError, 'historical archive identity'):
            self.verify(report)

    def test_changed_and_omitted_source_pins_are_rejected(self):
        for altered in ('changed', 'omitted'):
            report = copy.deepcopy(self.report)
            name = 'compat/bionic/posix_bionic.cc'
            if altered == 'changed':
                report['sources'][name] = '0' * 64
            else:
                del report['sources'][name]
            with self.subTest(altered=altered), \
                    self.assertRaisesRegex(AssertionError, 'historical source pins'):
                self.verify(report)

    def test_changed_measurement_summary_is_rejected(self):
        report = copy.deepcopy(self.report)
        report['final']['processes'] += 1
        with self.assertRaisesRegex(AssertionError, 'complete summary'):
            self.verify(report)


if __name__ == '__main__':
    unittest.main()
