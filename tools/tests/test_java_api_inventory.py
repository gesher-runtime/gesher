"""Synthetic DEX/archive cases; actual boot JARs are separate offline checks."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import java_api_inventory as audit


def dex(definitions=(2, 0)):
    descriptors = [b'Lsample/AChild;', b'Lsample/External;', b'Lsample/ZParent;']
    data_at = 136 + 32 * len(definitions)
    data = bytearray(data_at)
    data[:8] = b'dex\n039\0'
    struct.pack_into('<III', data, 36, 112, 0x12345678, 0)
    struct.pack_into('<II', data, 56, 3, 112)
    struct.pack_into('<II', data, 64, 3, 124)
    struct.pack_into('<II', data, 96, len(definitions), 136)
    for i, value in enumerate(descriptors):
        struct.pack_into('<I', data, 112 + 4*i, len(data))
        struct.pack_into('<I', data, 124 + 4*i, i)
        data += bytes([len(value)]) + value + b'\0'
    for i, value in enumerate(definitions):
        struct.pack_into('<IIIIIIII', data, 136 + 32*i, value, 1, 0xffffffff if value == 2 else 2,
                         0, 0xffffffff, 0, 0, 0)
    while len(data) % 4:
        data += b'\0'
    map_at = len(data)
    data += struct.pack('<I', 1) + struct.pack('<HHII', 0x1000, 0, 1, map_at)
    struct.pack_into('<I', data, 32, len(data))
    struct.pack_into('<I', data, 52, map_at)
    struct.pack_into('<II', data, 104, len(data) - data_at, data_at)
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into('<I', data, 8, zlib.adler32(data[12:]) & 0xffffffff)
    return bytes(data)


def classfile(name):
    pool = b''
    for index, value in enumerate((name.encode('ascii'), b'java/lang/Object'), 1):
        pool += struct.pack('>BH', 1, len(value)) + value
        pool += struct.pack('>BH', 7, index * 2 - 1)
    return b'\xca\xfe\xba\xbe' + struct.pack('>HHH', 0, 52, 5) + pool + struct.pack('>7H', 1, 2, 4, 0, 0, 0, 0)


def archive(path, entries):
    with zipfile.ZipFile(path, 'w') as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return path


class InventoryTests(unittest.TestCase):
    def test_dependency_order_and_only_definitions(self):
        names, version = audit.dex_classes(dex())
        self.assertEqual(names, {'sample.AChild', 'sample.ZParent'})
        self.assertNotIn('sample.External', names)
        self.assertEqual(version, '039')

    def test_duplicate_class_rejected(self):
        with self.assertRaisesRegex(audit.InvalidInput, 'invalid_dex_class_index'):
            audit.dex_classes(dex((2, 2)))

    def test_checksum_corruption_rejected(self):
        changed = bytearray(dex())
        changed[-1] ^= 1
        with self.assertRaisesRegex(audit.InvalidInput, 'checksum'):
            audit.dex_classes(bytes(changed))

    def test_truncated_header_rejected(self):
        with self.assertRaises(audit.InvalidInput):
            audit.dex_classes(dex()[:40])

    def test_multidex_union_counts_definitions(self):
        with tempfile.TemporaryDirectory() as tmp:
            jar = Path(tmp)/'framework.jar'
            with zipfile.ZipFile(jar, 'w') as z:
                z.writestr('classes.dex', dex())
                z.writestr('classes2.dex', dex())
            names, _, manifest = audit.archive_inventory(jar, 'boot', 'synthetic', audit.Limits())
            self.assertEqual(names, {'sample.AChild', 'sample.ZParent'})
            self.assertEqual(manifest['selected_members'], 2)

    def test_provider_root_dex_outside_pattern_fails_and_nested_dex_is_recorded(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = archive(Path(tmp)/'bad.apk', {'classes.dex': dex(), 'extra.dex': dex()})
            with self.assertRaisesRegex(audit.InvalidInput, 'unsupported_provider_dex_member_name'):
                audit.archive_inventory(bad, 'provider', 'provider-01', audit.Limits())
            good = archive(Path(tmp)/'good.apk', {'classes.dex': dex(), 'assets/plugin.dex': dex()})
            names, _, manifest = audit.archive_inventory(good, 'provider', 'provider-02', audit.Limits())
            self.assertEqual(names, {'sample.AChild', 'sample.ZParent'})
            self.assertEqual(manifest['skipped_nested_dex_members'], ['assets/plugin.dex'])

    def test_unsafe_member_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            jar = Path(tmp)/'framework.jar'
            with zipfile.ZipFile(jar, 'w') as z:
                z.writestr('../classes.dex', dex())
            with self.assertRaisesRegex(audit.InvalidInput, 'unsafe_zip_member'):
                audit.archive_inventory(jar, 'boot', 'synthetic', audit.Limits())


class ProviderInventoryTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        sdk_names = ['sample/AChild', 'sample/External', 'sample/Missing', 'sample/ZParent']
        self.sdk = archive(self.root / 'android.jar', {name + '.class': classfile(name) for name in sdk_names})
        self.boot = archive(self.root / 'framework.jar', {'classes.dex': dex()})
        self.module = archive(self.root / 'module.jar', {'classes.dex': dex((1,))})

    def test_unnamed_run_does_not_assess_outside_boot(self):
        report = audit.inventory(self.sdk, [self.boot])
        self.assertNotIn('outside_boot_providers', report)
        self.assertNotIn('outside_boot_provider_status_counts', report['installed'])
        for row in report['installed']['missing_classes']:
            self.assertEqual(row['outside_boot_provider_status'], 'not_assessed')

    def test_provider_definitions_are_attributed_without_changing_boot_counts(self):
        candidate = archive(self.root / 'candidate.jar', {'classes.dex': dex((2, 1))})
        split_base = archive(self.root / 'base=archive.apk', {'classes.dex': dex((1,))})
        split_code = archive(self.root / 'split.apk', {
            'classes.dex': dex((0,)), 'classes2.dex': dex((1,)), 'assets/other.dex': b'not in the APK class path',
        })
        output = self.root / 'out'
        run = subprocess.run([sys.executable, str(Path(audit.__file__)), '--sdk-jar', str(self.sdk),
                              '--boot-jar', str(self.boot), '--candidate-replace', f'{self.boot.name}={candidate}',
                              '--provider', f'module={self.module}', '--provider', f'app={split_base}',
                              '--provider', f'app={split_code}', '--outdir', str(output)],
                             text=True, capture_output=True, check=True)
        self.assertEqual(json.loads(run.stdout)['installed_available'], 2)
        report = json.loads((output / 'java-class-availability.json').read_text(encoding='utf-8'))
        installed = report['installed']
        self.assertEqual(installed['sdk_absent_from_boot'], 2)
        rows = {row['class']: row for row in installed['missing_classes']}
        external = rows['sample.External']
        self.assertEqual(external['outside_boot_provider_status'], 'defined_in_supplied_provider')
        self.assertEqual(external['outside_boot_definitions'], {
            'app': ['provider-02', 'provider-03'], 'module': ['provider-01'],
        })
        self.assertEqual(rows['sample.Missing']['outside_boot_provider_status'], 'absent_from_supplied_providers')
        self.assertEqual(rows['sample.Missing']['outside_boot_definitions'], {})
        self.assertEqual(installed['outside_boot_provider_status_counts'], {
            'defined_in_supplied_provider': 1, 'absent_from_supplied_providers': 1,
        })
        self.assertEqual(installed['boot_absent_sdk_classes_by_provider'], {'app': 1, 'module': 1})
        self.assertEqual(report['candidate']['boot_absent_sdk_classes_by_provider'], {'app': 1, 'module': 0})
        self.assertEqual(report['candidate']['missing_classes'][0]['outside_boot_definitions'], {'app': ['provider-03']})
        self.assertEqual(report['outside_boot_providers']['app'], {
            'input_source_ids': ['provider-02', 'provider-03'], 'defined_classes': 2, 'sdk_class_definitions': 2,
        })
        manifest = next(item for item in report['inputs'] if item['source_id'] == 'provider-03')
        self.assertEqual(manifest['sha256'], hashlib.sha256(split_code.read_bytes()).hexdigest())
        self.assertEqual(manifest['selected_members'], 2)
        self.assertNotIn(str(self.root), json.dumps(report))
        self.assertIn('Class-loader visibility, order and linkage are untested',
                      (output / 'java-class-availability.md').read_text(encoding='utf-8'))

    def test_extra_provider_uses_existing_archive_count_and_input_validation(self):
        with self.assertRaisesRegex(audit.InvalidInput, 'archive_count_or_duplicate_boot_name'):
            audit.inventory(self.sdk, [self.boot], limits=audit.Limits(max_archives=2),
                            providers=[('module', self.module)])
        with self.assertRaisesRegex(audit.InvalidInput, 'invalid_provider_label'):
            audit.inventory(self.sdk, [self.boot], providers=[('path/label', self.module)])
        malformed = archive(self.root / 'invalid.apk', {'classes2.dex': dex()[:40]})
        with self.assertRaisesRegex(audit.InvalidInput, 'out_of_bounds'):
            audit.inventory(self.sdk, [self.boot], providers=[('app', malformed)])


if __name__ == '__main__':
    unittest.main()
