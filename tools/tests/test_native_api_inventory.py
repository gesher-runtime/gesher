"""Export-list parsing preserves binding, versions and executable scope."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from native_api_inventory import parse_exports


class ExportTests(unittest.TestCase):
    def test_defined_public_exports_keep_binding_type_and_versions(self):
        rows=parse_exports('''
Num: Value Size Type Bind Vis Ndx Name
1: 00001000 1 FUNC GLOBAL DEFAULT 9 foo@@LIBC_N
2: 00001010 8 OBJECT WEAK DEFAULT 10 weak_data@LIBC
3: 00001020 1 FUNC GLOBAL PROTECTED 9 protected_fn
4: 00000000 0 FUNC GLOBAL DEFAULT UND imported
5: 00001030 1 FUNC GLOBAL HIDDEN 9 hidden_fn
6: 00000000 0 FILE LOCAL DEFAULT ABS source.c
''')
        self.assertEqual([r['symbol'] for r in rows],['foo','weak_data','protected_fn'])
        self.assertEqual(rows[0]['versioned_name'],'foo@@LIBC_N')
        self.assertEqual((rows[1]['binding'],rows[1]['type']),('WEAK','OBJECT'))

    def test_malformed_export_identifier_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'Unexpected export identifier'):
            parse_exports('1: 00001000 1 FUNC GLOBAL DEFAULT 9 wrong/name')

    def test_launcher_entry_is_not_silently_removed_from_reference(self):
        rows=parse_exports('1: 00001000 1 FUNC GLOBAL DEFAULT 9 __libc_init@@LIBC')
        self.assertEqual(rows[0]['symbol'],'__libc_init')


if __name__=='__main__':unittest.main()
