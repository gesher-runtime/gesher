"""Exact feature receipts survive an isolated checkout without donor objects."""
import copy
import hashlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('durable_selected_policy', ROOT / 'tools/net-tcp-priority/test-selected-policy.py')
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class SourceProjectionDurabilityTests(unittest.TestCase):
    def test_isolated_repository_without_feature_objects(self):
        paths = set(policy.APPCONTAINER_INTEGRATION_PATHS) | {
            policy.APPCONTAINER_INTEGRATION_PATCH, policy.APPCONTAINER_INTEGRATION_QUALIFICATION,
            policy.NATIVE2_PROJECTION_ADAPTER, policy.FEATURE_SOURCE_ARCHIVE,
            'compat/bionic/posix_bionic.cc',
            'tools/net-tcp-priority/qualified/scandir-native2-source.json',
            'tools/net-tcp-priority/qualified/scandir-native2-source.patch',
            'tools/net-tcp-priority/qualified/appcontainer-identity-source.patch',
            'tools/net-tcp-priority/qualified/appcontainer-property-source.patch',
        }
        with tempfile.TemporaryDirectory(prefix='gesher-feature-objects-absent-') as temporary:
            isolated = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(isolated)], check=True, capture_output=True)
            for path in paths:
                target = isolated / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / path).read_bytes())
            for revision in (policy.APPCONTAINER_INTEGRATION_BASE,
                             policy.APPCONTAINER_IDENTITY_DONOR, policy.APPCONTAINER_PROPERTY_DONOR,
                             *policy.FEATURE_SOURCE_ADDITIONAL_SHA256,
                             '3c7fe0dd2813cd6495e303405b974fc46dfea30f'):
                self.assertNotEqual(subprocess.run(['git', '-C', str(isolated), 'cat-file', '-e', revision],
                                                  capture_output=True).returncode, 0)
            with patch.object(policy, 'ROOT', isolated):
                previous = policy.preceding_appcontainer_integration_sources(
                    policy.APPCONTAINER_INTEGRATION_QUALIFICATION_SHA256)
                self.assertEqual(len(previous), len(policy.APPCONTAINER_INTEGRATION_PATHS))
                policy.check_appcontainer_donor_receipts()
                projected, native2, current, baseline, manifest, delta = policy.native2_projection()
                self.assertEqual(native2.sha(projected[native2.PATH]), native2.BASELINE_SHA256)
                self.assertEqual(native2.negative_controls(current, baseline, manifest, delta), 11)
                sources = policy.feature_snapshot_sources()
                for revision, pins in policy.FEATURE_SOURCE_ADDITIONAL_SHA256.items():
                    for path in pins:
                        self.assertEqual(policy.usb_historical_source(revision, path), sources[revision + '/' + path])
                policy.appcontainer_integration_negative_controls(
                    policy.APPCONTAINER_INTEGRATION_QUALIFICATION_SHA256)

    def test_source_archive_rejects_corruption(self):
        raw = (ROOT / policy.FEATURE_SOURCE_ARCHIVE).read_bytes()
        with self.assertRaisesRegex(AssertionError, 'archive digest'):
            policy.feature_snapshot_sources(raw + b'altered')

    def test_repinned_source_archive_keeps_exact_members_and_bytes(self):
        raw = (ROOT / policy.FEATURE_SOURCE_ARCHIVE).read_bytes()
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            sources = {name: archive.read(name) for name in archive.namelist()}
        for mutation in ('missing', 'extra', 'changed'):
            modified = copy.deepcopy(sources)
            name = sorted(modified)[0]
            if mutation == 'missing':
                del modified[name]
            elif mutation == 'extra':
                modified['unexpected/source.cc'] = b'unqualified'
            else:
                modified[name] += b'changed'
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
                for name, data in modified.items():
                    archive.writestr(name, data)
            altered = buffer.getvalue()
            with self.subTest(mutation=mutation), self.assertRaisesRegex(AssertionError,
                    'archive bytes' if mutation == 'changed' else 'exact members'):
                policy.feature_snapshot_sources(altered, hashlib.sha256(altered).hexdigest())


if __name__ == '__main__':
    unittest.main()
