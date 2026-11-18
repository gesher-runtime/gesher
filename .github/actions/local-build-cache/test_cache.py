"""Exercise the real cache implementation only in isolated temporary fixtures."""
import importlib.util
import io
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('local_build_cache', Path(__file__).with_name('cache.py'))
cache_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache_module)


class CacheContracts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.workspace = self.root / 'workspace'
        self.workspace.mkdir()
        self.cache = cache_module.DiskCache(self.root / 'cache', self.workspace)
        self.source = self.workspace / 'third_party' / 'aosp'
        self.source.mkdir(parents=True)
        (self.source / 'source.c').write_text('exact pinned source')
        self.names = ['third_party/aosp']

    def entry(self):
        return next(self.cache.entries(self.cache.bucket(self.names)))

    def test_exact_restore_replaces_stale_files_and_preserves_source_bytes(self):
        self.assertTrue(self.cache.save(self.names, 'aosp-lock-A'))
        (self.source / 'source.c').write_text('changed')
        (self.source / 'stale').write_text('must disappear')
        self.assertEqual(self.cache.restore(self.names, 'aosp-lock-A'), 'aosp-lock-A')
        self.assertEqual((self.source / 'source.c').read_text(), 'exact pinned source')
        self.assertFalse((self.source / 'stale').exists())

    def test_prefix_restore_reports_actual_key_not_exact_hit(self):
        self.cache.save(self.names, 'aosp-lock-A')
        self.assertEqual(self.cache.restore(self.names, 'aosp-lock-B', ['aosp-']), 'aosp-lock-A')
        self.assertEqual(self.cache.restore(self.names, 'different'), '')

    def test_snapshot_is_immutable_and_not_a_link_to_live_sources(self):
        self.cache.save(self.names, 'aosp-lock-A')
        (self.source / 'source.c').write_text('new bytes')
        self.assertFalse(self.cache.save(self.names, 'aosp-lock-A'))
        self.cache.restore(self.names, 'aosp-lock-A')
        self.assertEqual((self.source / 'source.c').read_text(), 'exact pinned source')

    def test_corrupt_payload_rejected_before_workspace_changes(self):
        self.cache.save(self.names, 'aosp-lock-A')
        (self.entry() / 'payload.tar').write_bytes(b'corrupt')
        with self.assertRaisesRegex(cache_module.CacheError, 'checksum'):
            self.cache.restore(self.names, 'aosp-lock-A')
        self.assertEqual((self.source / 'source.c').read_text(), 'exact pinned source')

    def test_traversal_archive_rejected_even_with_matching_digest(self):
        self.cache.save(self.names, 'aosp-lock-A')
        entry = self.entry()
        with tarfile.open(entry / 'payload.tar', 'w:') as archive:
            member = tarfile.TarInfo('entry-0/../../escape')
            member.size = 1
            archive.addfile(member, io.BytesIO(b'x'))
        manifest = json.loads((entry / 'manifest.json').read_text())
        manifest['sha256'] = cache_module.digest_file(entry / 'payload.tar')
        (entry / 'manifest.json').write_text(json.dumps(manifest))
        with self.assertRaises(cache_module.CacheError):
            self.cache.restore(self.names, 'aosp-lock-A')
        self.assertFalse((self.root / 'escape').exists())

    def test_paths_outside_workspace_or_globs_are_rejected(self):
        for name in ('../outside', '/absolute', '.', 'out/*', 'out/../outside'):
            with self.subTest(name=name), self.assertRaises(cache_module.CacheError):
                self.cache.save([name], 'key')

    def test_different_path_versions_do_not_cross_restore(self):
        self.cache.save(self.names, 'same-key')
        self.assertEqual(self.cache.restore(['out/framework'], 'same-key'), '')

    def test_other_pr_cannot_restore_an_identical_key(self):
        self.cache.save(self.names, 'same-key')
        other = cache_module.DiskCache(self.root / 'cache', self.workspace, namespace='another-pr')
        self.assertEqual(other.restore(self.names, 'same-key', ['same-']), '')

    def test_absent_optional_path_is_not_recorded_as_a_cache_hit(self):
        self.assertFalse(self.cache.save(['out/not-built'], 'key'))
        self.assertEqual(self.cache.restore(['out/not-built'], 'key'), '')

    def test_oversized_snapshot_is_not_published(self):
        tiny = cache_module.DiskCache(self.root / 'tiny', self.workspace, budget=1)
        self.assertFalse(tiny.save(self.names, 'key'))
        self.assertEqual(tiny.restore(self.names, 'key'), '')

    def test_interrupted_snapshot_is_removed_without_becoming_a_hit(self):
        interrupted = self.cache.bucket(self.names) / 'save-interrupted'
        interrupted.mkdir()
        (interrupted / 'partial').write_text('partial bytes')
        self.assertEqual(self.cache.restore(self.names, 'key'), '')
        self.assertFalse(interrupted.exists())

    @unittest.skipIf(__import__('sys').platform == 'win32', 'Windows symlink privilege is not required')
    def test_symlink_destination_cannot_remove_outside_directory(self):
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'keep').write_text('keep')
        self.cache.save(self.names, 'key')
        shutil.rmtree(self.source)
        self.source.symlink_to(outside, target_is_directory=True)
        with self.assertRaises(cache_module.CacheError):
            self.cache.restore(self.names, 'key')
        self.assertEqual((outside / 'keep').read_text(), 'keep')


if __name__ == '__main__':
    unittest.main()
