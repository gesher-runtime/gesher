"""Source-only adversarial fixtures for LLD's published COFF map format.

Format reference: llvm-project/lld/COFF/MapFile.cpp (Lib:Object, extension stripped).
The symbols are deliberately synthetic; these are parser controls, not CI evidence.
"""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('font_link', Path(__file__).with_name('verify-standalone-link.py'))
proof = importlib.util.module_from_spec(spec)
spec.loader.exec_module(proof)

MAP = '''
 Address         Publics by Value              Rva+Base               Lib:Object
 0001:00001000   fixture_native               0000000140001000        native_fonts.cc.obj
 0001:00002000   fixture_collection           0000000140002000        libminikin:FontCollection.cpp.obj
 0001:00003000   fixture_skia                 0000000140003000        libhwui:MinikinSkia.cpp.obj
'''
COMMAND = ': && clang++ object.obj -o gesher-exec.exe libminikin.a libhwui.a && python proof.py'


class FormatControls(unittest.TestCase):
    def test_primary_coff_format(self):
        rows = proof.check_map(MAP)
        self.assertEqual(rows[1], ('fixture_collection', 'libminikin:FontCollection.cpp.obj'))
        self.assertEqual(len(rows), 3)

    def test_missing_origin(self):
        with self.assertRaises(AssertionError):
            proof.check_map(MAP.replace('libminikin:FontCollection.cpp.obj', ''))

    def test_wrong_archive_or_member(self):
        for origin in ('notlibminikin:FontCollection.cpp.obj', 'libminikin:Other.cpp.obj',
                       'libminikin.a:FontCollection.cpp.obj', 'FontCollection.cpp.obj',
                       'libminikin:FontCollection.cpp.obj.evil'):
            with self.subTest(origin=origin), self.assertRaises(AssertionError):
                proof.check_map(MAP.replace('libminikin:FontCollection.cpp.obj', origin))

    def test_vm_suffix_origins_and_entry(self):
        for symbol, origin in (('foo', 'libart_runtime:thread.obj'),
                               ('foo', 'libart_runtime:thread.obj.evil'),
                               ('JNI_CreateJavaVM', 'innocent.obj')):
            with self.subTest(origin=origin), self.assertRaises(AssertionError):
                proof.check_map(MAP + f' 0001:00004000 {symbol} 0000000140004000 {origin}\n')

    def test_link_command_and_same_file_repetition(self):
        argv, archives = proof.link_inputs(COMMAND.replace('libhwui.a', 'libhwui.a libminikin.a'),
                                          Path('clang++'), Path.cwd())
        self.assertEqual(archives['libminikin.a'].name, 'libminikin.a')
        self.assertNotIn('python', argv)

    def test_colliding_archives_response_file_and_vm(self):
        for addition in (' other/libminikin.a', ' @hidden.rsp', ' libart_runtime.a',
                         ' libart_runtime.a.evil', ' CMakeFiles/framework_runtime.dir/file.obj'):
            command = COMMAND.replace(' && python', addition + ' && python')
            with self.subTest(addition=addition), self.assertRaises(AssertionError):
                proof.link_inputs(command, Path('clang++'), Path.cwd())

    def test_multiple_commands(self):
        with self.assertRaises(AssertionError):
            proof.link_inputs(COMMAND + '\n' + COMMAND, Path('clang++'), Path.cwd())


if __name__ == '__main__':
    unittest.main()