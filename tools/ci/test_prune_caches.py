"""Which cache entries tools/ci/prune_caches.py deletes."""
from datetime import datetime, timedelta, timezone
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prune_caches import family, superseded  # noqa: E402

NOW = datetime(2026, 11, 22, 3, 0, tzinfo=timezone.utc)
SHA_A, SHA_B = 'a' * 40, 'b' * 40


def entry(id, key, minutes_ago, ref='refs/heads/main', used_minutes_ago=None):
    stamp = lambda m: (NOW - timedelta(minutes=m)).isoformat().replace('+00:00', 'Z')
    return dict(id=id, key=key, ref=ref, size_in_bytes=1, created_at=stamp(minutes_ago),
                last_accessed_at=stamp(minutes_ago if used_minutes_ago is None else used_minutes_ago))


def ids(caches):
    return [cache['id'] for cache in superseded(caches, NOW)]


class PruneCaches(unittest.TestCase):
    def test_families(self):
        self.assertEqual(family(f'ccache-runtime-{SHA_A}'), 'ccache-runtime-')
        self.assertEqual(family('ccache-host-tools-44975994107'), 'ccache-host-tools-')
        self.assertEqual(
            family(f'codeql-overlay-base-database-1-8a3d-cpp-2.27.5-{SHA_A}-44975994006-1'),
            'codeql-overlay-base-database-1-8a3d-cpp-2.27.5-')
        for key in (f'combined-ndk-r27c-{SHA_A}', 'ndk-27.2.12479018', f'aosp-{SHA_A}',
                    f'coremod-{SHA_A}', 'framework-jvm-tools-kotlin-2.1.21', 'ccache-runtime-'):
            self.assertIsNone(family(key), key)

    def test_keeps_newest_per_ref_and_family(self):
        caches = [
            entry(1, f'ccache-runtime-{SHA_A}', 120),
            entry(2, f'ccache-runtime-{SHA_B}', 60),
            entry(3, f'ccache-posix-{SHA_A}', 120),
            entry(4, f'ccache-runtime-{SHA_A}', 120, ref='refs/pull/7/merge'),
            entry(5, f'aosp-{SHA_A}', 300),
            entry(6, f'aosp-{SHA_B}', 30),
        ]
        self.assertEqual(ids(caches), [1])

    def test_spares_recently_used_entries(self):
        caches = [entry(1, 'ccache-host-tools-1', 120, used_minutes_ago=5),
                  entry(2, 'ccache-host-tools-2', 60)]
        self.assertEqual(ids(caches), [])

    def test_codeql_languages_are_separate_families(self):
        caches = [entry(1, f'codeql-overlay-base-database-1-x-cpp-2.27.5-{SHA_A}-1-1', 120),
                  entry(2, f'codeql-overlay-base-database-1-y-java-2.27.5-{SHA_B}-2-1', 60),
                  entry(3, f'codeql-overlay-base-database-1-x-cpp-2.27.5-{SHA_B}-2-1', 60)]
        self.assertEqual(ids(caches), [1])


if __name__ == '__main__':
    unittest.main()
