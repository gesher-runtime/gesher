"""CI-only fixtures for AOSP cache ownership and process/lock boundaries."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('aosp_cache', Path(__file__).parents[1] / 'aosp_cache.py')
CACHE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CACHE)


@unittest.skipUnless(os.environ.get('GITHUB_ACTIONS') == 'true' and sys.platform == 'linux',
                     'CI-only fixture execution')
class CacheBoundaryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.workspace = Path(self.temp.name) / 'workspace'
        self.root = self.workspace / 'third_party' / 'aosp'
        self.project = self.root / 'system' / 'connectivity' / 'wificond'
        self.git = self.project / '.git'
        (self.git / 'info').mkdir(parents=True)
        self.environment = mock.patch.dict(os.environ, {
            'GITHUB_ACTIONS': 'true', 'GITHUB_WORKSPACE': str(self.workspace)})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def stale(self):
        lock = self.git / 'info' / 'sparse-checkout.lock'
        lock.write_text('interrupted sparse transaction\n')
        return lock

    def test_restored_sparse_lock_removed_before_git(self):
        lock = self.stale()
        CACHE.guard('prepare', self.root)
        self.assertFalse(lock.exists())
        CACHE.guard('snapshot', self.root)

    def test_snapshot_rejects_lock_without_removing_it(self):
        lock = self.stale()
        with self.assertRaisesRegex(CACHE.CacheError, 'poisoned'):
            CACHE.guard('snapshot', self.root)
        self.assertTrue(lock.exists())

    def test_active_real_git_refuses_cleanup_and_snapshot(self):
        subprocess.run(['git', 'init', '-q', str(self.project)], check=True)
        lock = self.stale()
        process = subprocess.Popen(['git', 'hash-object', '--stdin'], cwd=self.project,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline and not CACHE.git_processes(self.root):
                time.sleep(0.01)
            self.assertIsNone(process.poll())
            self.assertTrue(CACHE.git_processes(self.root))
            for phase in ('prepare', 'snapshot'):
                with self.assertRaisesRegex(CACHE.CacheError, 'active AOSP Git'):
                    CACHE.guard(phase, self.root)
            self.assertTrue(lock.exists())
        finally:
            try:
                process.communicate(b'fixture\n', timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=5)

    def test_external_root_and_local_execution_refused(self):
        with self.assertRaises(CACHE.CacheError):
            CACHE.guard('prepare', self.workspace.parent)
        with mock.patch.dict(os.environ, {'GITHUB_ACTIONS': 'false'}):
            with self.assertRaises(CACHE.CacheError):
                CACHE.guard('prepare', self.root)

    def test_symlink_lock_cannot_touch_external_file(self):
        outside = self.workspace / 'outside'
        outside.write_text('retain')
        (self.git / 'info' / 'sparse-checkout.lock').symlink_to(outside)
        with self.assertRaises(CACHE.CacheError):
            CACHE.guard('prepare', self.root)
        self.assertEqual(outside.read_text(), 'retain')

    def test_unknown_metadata_lock_is_not_deleted(self):
        lock = self.git / 'unknown.lock'
        lock.write_text('retain')
        with self.assertRaisesRegex(CACHE.CacheError, 'unrecognized'):
            CACHE.guard('prepare', self.root)
        self.assertTrue(lock.exists())

    def test_metadata_directory_symlink_is_refused(self):
        outside = self.workspace / 'outside-info'
        outside.mkdir()
        (outside / 'sparse-checkout.lock').write_text('retain')
        (self.git / 'info').rmdir()
        (self.git / 'info').symlink_to(outside, target_is_directory=True)
        with self.assertRaises(CACHE.CacheError):
            CACHE.guard('prepare', self.root)
        self.assertEqual((outside / 'sparse-checkout.lock').read_text(), 'retain')


if __name__ == '__main__':
    unittest.main()
