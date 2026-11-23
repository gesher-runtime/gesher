"""Metadata-only controls: these synthetic text files are not executable proof."""
import importlib.util
import json
from pathlib import Path
import re
import shutil
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('capture_role_under_test', Path(__file__).with_name('host_roles.py'))
roles = importlib.util.module_from_spec(spec)
spec.loader.exec_module(roles)


class CaptureRoles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'; self.build.mkdir()
        self.dist = self.root / 'dist'; self.dist.mkdir()
        self.output = self.root / 'fixture'
        self.state = self.root / 'state.json'
        self.info = {'source': '1' * 40, 'run': '1', 'attempt': '1',
                     'repository': 'owner/repo', 'repository_id': '7', 'event': 'pull_request',
                     'supplier_head': '2' * 40, 'head_repository_id': '8'}
        for name in roles.FILES:
            (self.dist / name).write_bytes(b'SYNTHETIC_TEXT_NOT_PE normal ' + name.encode())
        self.context = patch.object(roles.shared, 'context', return_value=self.info); self.context.start()
        self.addCleanup(self.context.stop)
        self.compiler = patch.object(roles.shared, 'compiler', return_value={'sha256': '2' * 64})
        self.compiler_mock = self.compiler.start(); self.addCleanup(self.compiler.stop)
        self.configure(False)

    def configure(self, enabled, other=False):
        (self.build / 'CMakeCache.txt').write_text('GESHER_CAPTURE_TEST_SOURCES:BOOL=' + ('ON' if enabled else 'OFF')
                + '\nGESHER_WEB3_QUERY_TESTS:BOOL=' + ('ON' if other else 'OFF') + '\n')
        args = ['compiler', '-o', 'capture.obj', '-c', 'capture.cc']
        if enabled: args.insert(1, '-DGESHER_CAPTURE_TEST_SOURCES=1')
        rows = [{'directory': str(self.build), 'file': str(roles.shared.ROOT / 'compat/media/audio/capture.cc'), 'arguments': args},
                {'directory': str(self.build), 'file': str(roles.shared.ROOT / 'compat/media/audio/engine.cc'),
                 'arguments': ['compiler', '-o', 'engine.obj', '-c', 'engine.cc']}]
        (self.build / 'compile_commands.json').write_text(json.dumps(rows))
        for name in roles.FILES:
            (self.build / name).write_bytes(b'SYNTHETIC_TEXT_NOT_PE fixture ' + (roles.SENTINEL if enabled else b''))

    def seal(self):
        roles.production(self.build, self.dist, self.state)
        self.configure(True)

    def publish(self):
        roles.fixture(self.build, self.dist, self.state, self.output)
        for path in self.output.iterdir(): shutil.copy2(path, self.dist / path.name)

    def test_complete_separate_roles(self):
        self.seal(); self.publish(); roles.verify(self.dist, True)
        self.assertNotEqual((self.dist / 'dalvikvm.exe').read_bytes(),
                            (self.dist / roles.FILES['dalvikvm.exe']).read_bytes())

    def test_production_rejects_enabled_flag(self):
        self.configure(True)
        with self.assertRaises(SystemExit): roles.production(self.build, self.dist, self.state)

    def test_production_rejects_fixture_bytes(self):
        (self.dist / 'dalvikvm.exe').write_bytes(roles.SENTINEL)
        with self.assertRaises(SystemExit): roles.production(self.build, self.dist, self.state)

    def test_unrelated_flag_change_rejected(self):
        self.seal()
        file = self.build / 'compile_commands.json'; rows = json.loads(file.read_text())
        rows[1]['arguments'].append('-DUNRELATED=1'); file.write_text(json.dumps(rows))
        with self.assertRaises(SystemExit): self.publish()

    def test_mixed_fixtures_rejected(self):
        self.seal(); self.configure(True, other=True)
        with self.assertRaises(SystemExit): self.publish()

    def test_changed_production_rejected(self):
        self.seal(); (self.dist / 'media_probe.exe').write_bytes(b'changed')
        with self.assertRaises(SystemExit): self.publish()

    def test_wrong_run_rejected(self):
        self.seal(); self.info['attempt'] = '2'
        with self.assertRaises(SystemExit): self.publish()

    def test_wrong_compiler_rejected(self):
        self.seal(); self.compiler_mock.return_value = {'sha256': '3' * 64}
        with self.assertRaises(SystemExit): self.publish()

    def test_missing_fixture_sentinel_rejected(self):
        self.seal(); (self.build / 'media_probe.exe').write_bytes(b'normal')
        with self.assertRaises(SystemExit): self.publish()

    def test_changed_fixture_rejected(self):
        self.seal(); self.publish(); (self.dist / roles.FILES['dalvikvm.exe']).write_bytes(roles.SENTINEL + b'changed')
        with self.assertRaises(SystemExit): roles.verify(self.dist, True)

    def test_incomplete_production_inventory_rejected(self):
        self.seal(); self.publish()
        path = self.dist / 'media-capture-production-role.json'; role = json.loads(path.read_text())
        role['files'].pop('media_probe.exe'); path.write_text(json.dumps(role))
        with self.assertRaises(SystemExit): roles.verify(self.dist, True)

    def test_services_checkout_contains_role_verifier(self):
        workflow = (roles.shared.ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        job = re.search(r'(?ms)^  test-shards:.*?(?=^  [a-zA-Z0-9_-]+:|\Z)', workflow).group()
        checkout = job.split('sparse-checkout: |', 1)[1].split('\n      - ', 1)[0]
        self.assertIn('tools/media_capture', checkout.split())
        self.assertIn('--name media-capture-test --producer "build"', job)


if __name__ == '__main__': unittest.main()
