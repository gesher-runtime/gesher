"""Synthetic DEX/archive cases; actual boot JARs are separate offline checks."""
from pathlib import Path
import hashlib
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import java_api_inventory as audit


def dex(definitions=(2, 0)):
    descriptors = [b'Lsample/AChild;', b'Lsample/External;', b'Lsample/ZParent;']
    data = bytearray(200)
    data[:8] = b'dex\n039\0'
    struct.pack_into('<III', data, 36, 112, 0x12345678, 0)
    struct.pack_into('<II', data, 56, 3, 112)
    struct.pack_into('<II', data, 64, 3, 124)
    struct.pack_into('<II', data, 96, 2, 136)
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
    struct.pack_into('<II', data, 104, len(data)-200, 200)
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into('<I', data, 8, zlib.adler32(data[12:]) & 0xffffffff)
    return bytes(data)


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

    def test_unsafe_member_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            jar = Path(tmp)/'framework.jar'
            with zipfile.ZipFile(jar, 'w') as z:
                z.writestr('../classes.dex', dex())
            with self.assertRaisesRegex(audit.InvalidInput, 'unsafe_zip_member'):
                audit.archive_inventory(jar, 'boot', 'synthetic', audit.Limits())


if __name__ == '__main__':
    unittest.main()
