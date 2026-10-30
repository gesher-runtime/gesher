"""Actual trimmed GNU -Map golden rows and semantic provenance controls.

Golden source: run41056309829/job124064608623, immutable compiled cd20.
The old strict proof failed; these real rows do not qualify runtime behavior.
"""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('font_link', Path(__file__).with_name('verify-standalone-link.py'))
proof = importlib.util.module_from_spec(spec)
spec.loader.exec_module(proof)

ACTUAL_MAP_SHA256 = '24c0b6009ff5225427cb464f6d1c6806bd0d49c47944fc488b901be1a8f6c87e'
GOLDEN_SHA256 = '6ccbd495b2ad2460fd62241327faf061d0e1a1d487ac8394e9bd18398243bab5'
GOLDEN = r'''Address  Size     Align Out     In      Symbol
00001000 00a20bb6  4096 .text
00001470 00004416    16         CMakeFiles/gesher_native_fonts.dir/compat/bionic/native_fonts.cc.obj:(.text)
00001470 00000000     0                 gesher::bionic::(anonymous namespace)::OpenIterator()
000018d0 00000000     0                 gesher::bionic::(anonymous namespace)::NextFont(gesher::bionic::(anonymous namespace)::Iterator*)
000019b0 00000000     0                 gesher::bionic::(anonymous namespace)::CloseIterator(gesher::bionic::(anonymous namespace)::Iterator*)
00001a10 00000000     0                 gesher::bionic::(anonymous namespace)::CreateMatcher()
00001a40 00000000     0                 gesher::bionic::(anonymous namespace)::DestroyMatcher(gesher::bionic::(anonymous namespace)::Matcher*)
00001a60 00000000     0                 gesher::bionic::(anonymous namespace)::SetStyle(gesher::bionic::(anonymous namespace)::Matcher*, unsigned short, bool)
00001a70 00000000     0                 gesher::bionic::(anonymous namespace)::SetLocales(gesher::bionic::(anonymous namespace)::Matcher*, char const*)
00001b20 00000000     0                 gesher::bionic::(anonymous namespace)::SetVariant(gesher::bionic::(anonymous namespace)::Matcher*, unsigned int)
00001b30 00000000     0                 gesher::bionic::(anonymous namespace)::Match(gesher::bionic::(anonymous namespace)::Matcher const*, char const*, unsigned short const*, unsigned int, unsigned int*)
000aa620 000009e9    16         MinikinSkia.cpp.obj:(.text)
000aa620 00000000     0                 android::MinikinFontSkia::MinikinFontSkia(sk_sp<SkTypeface>, int, void const*, unsigned long long, std::__1::basic_string_view<char, std::__1::char_traits<char>>, int, minikin::SortedPackedVector<minikin::FontVariation, 2ull, unsigned short> const&)
000aa620 00000000     0                 android::MinikinFontSkia::MinikinFontSkia(sk_sp<SkTypeface>, int, void const*, unsigned long long, std::__1::basic_string_view<char, std::__1::char_traits<char>>, int, minikin::SortedPackedVector<minikin::FontVariation, 2ull, unsigned short> const&)
000aa620 00000000     0                 android::MinikinFontSkia::MinikinFontSkia(sk_sp<SkTypeface>, int, void const*, unsigned long long, std::__1::basic_string_view<char, std::__1::char_traits<char>>, int, minikin::SortedPackedVector<minikin::FontVariation, 2ull, unsigned short> const&)
000aa790 00000000     0                 android::MinikinFontSkia::GetHorizontalAdvance(unsigned int, minikin::MinikinPaint const&, minikin::FontFakery const&) const
000aa790 00000000     0                 android::MinikinFontSkia::GetHorizontalAdvance(unsigned int, minikin::MinikinPaint const&, minikin::FontFakery const&) const
000aa860 00000000     0                 android::MinikinFontSkia_SetSkiaFont(minikin::MinikinFont const*, SkFont*, minikin::MinikinPaint const&, minikin::FontFakery const&)
000aaa20 00000000     0                 android::MinikinFontSkia::GetHorizontalAdvances(unsigned short*, unsigned int, minikin::MinikinPaint const&, minikin::FontFakery const&, float*) const
000aaa20 00000000     0                 android::MinikinFontSkia::GetHorizontalAdvances(unsigned short*, unsigned int, minikin::MinikinPaint const&, minikin::FontFakery const&, float*) const
000aaae0 00000000     0                 android::MinikinFontSkia::GetBounds(minikin::MinikinRect*, unsigned int, minikin::MinikinPaint const&, minikin::FontFakery const&) const
000afd60 00003c48    16         FontCollection.cpp.obj:(.text)
000afd60 00000000     0                 minikin::FontCollection::create(std::__1::shared_ptr<minikin::FontFamily>&&)
000afeb0 00000000     0                 minikin::FontCollection::create(std::__1::vector<std::__1::shared_ptr<minikin::FontFamily>, std::__1::allocator<std::__1::shared_ptr<minikin::FontFamily>>> const&)
000afeb0 00000000     0                 minikin::FontCollection::create(std::__1::vector<std::__1::shared_ptr<minikin::FontFamily>, std::__1::allocator<std::__1::shared_ptr<minikin::FontFamily>>> const&)
000aff90 00000000     0                 minikin::FontCollection::FontCollection(std::__1::vector<std::__1::shared_ptr<minikin::FontFamily>, std::__1::allocator<std::__1::shared_ptr<minikin::FontFamily>>> const&)
000aff90 00000000     0                 minikin::FontCollection::FontCollection(std::__1::vector<std::__1::shared_ptr<minikin::FontFamily>, std::__1::allocator<std::__1::shared_ptr<minikin::FontFamily>>> const&)
000b0040 00000000     0                 minikin::FontCollection::init(std::__1::vector<std::__1::shared_ptr<minikin::FontFamily>, std::__1::allocator<std::__1::shared_ptr<minikin::FontFamily>>> const&)
000b06e0 00000000     0                 minikin::FontCollection::FontCollection(minikin::BufferReader*, std::__1::shared_ptr<std::__1::vector<std::__1::shared_ptr<minikin::FontFamily>, std::__1::allocator<std::__1::shared_ptr<minikin::FontFamily>>>> const&)
000b06e0 00000000     0                 minikin::FontCollection::FontCollection(minikin::BufferReader*, std::__1::shared_ptr<std::__1::vector<std::__1::shared_ptr<minikin::FontFamily>, std::__1::allocator<std::__1::shared_ptr<minikin::FontFamily>>>> const&)
000b0950 00000000     0                 minikin::FontCollection::writeTo(minikin::BufferWriter*, std::__1::unordered_map<std::__1::shared_ptr<minikin::FontFamily>, unsigned int, std::__1::hash<std::__1::shared_ptr<minikin::FontFamily>>, std::__1::equal_to<std::__1::shared_ptr<minikin::FontFamily>>, std::__1::allocator<std::__1::pair<std::__1::shared_ptr<minikin::FontFamily> const, unsigned int>>> const&) const
'''

COMMAND = ': && clang++ object.obj -o gesher-exec.exe libminikin.a libhwui.a && python proof.py'


class ActualMapControls(unittest.TestCase):
    def test_actual_golden_symbols_and_owner(self):
        import hashlib
        self.assertEqual(hashlib.sha256(GOLDEN.encode()).hexdigest(), GOLDEN_SHA256)
        rows = proof.check_map(GOLDEN)
        self.assertEqual(len(rows), 27)
        expected = 'minikin::FontCollection::create(std::__1::shared_ptr<minikin::FontFamily>&&)'
        self.assertIn((expected, 'FontCollection.cpp.obj'), rows)
        self.assertEqual(proof.mapped_definitions(rows, 'FontCollection.cpp.obj', {expected}), [expected])
        self.assertEqual(proof.mapped_definitions(rows, 'FontCollection.cpp.obj', {'unrelated::create()'}), [])

    def test_contrasting_coff_map_rejected(self):
        with self.assertRaises(AssertionError):
            proof.check_map('Address Publics by Value Rva+Base Lib:Object\n0001:00001000 symbol 0000000140001000 libminikin:FontCollection.cpp.obj\n')

    def test_wrong_or_missing_owner(self):
        for replacement in ('Other.cpp.obj', 'FontCollection.cpp.obj.evil'):
            with self.subTest(replacement=replacement), self.assertRaises(AssertionError):
                proof.check_map(GOLDEN.replace('FontCollection.cpp.obj', replacement))
        with self.assertRaises(AssertionError):
            proof.check_map('\n'.join(line for line in GOLDEN.splitlines() if 'FontCollection.cpp.obj:(' not in line)+'\n')

    def test_parent_ranges_and_output_reset(self):
        for text in (GOLDEN.replace('000aa620 000009e9', '000aa620 00000001'),
                     GOLDEN.replace('000afd60 00000000', 'ffffffff 00000000'),
                     GOLDEN.replace('000afd60 00003c48', 'ffffffff 00003c48'),
                     GOLDEN.replace('000afd60 00000000', '00001000 00000010  4096 .other\n000afd60 00000000', 1)):
            with self.subTest(text=text[-160:]), self.assertRaises(AssertionError):
                proof.check_map(text)

    def test_vm_origin_suffix_and_entry(self):
        for symbol, origin in (('foo', 'libart_runtime.a.evil'), ('JNI_CreateJavaVM', 'innocent.obj')):
            text = GOLDEN + f'000c0000 00000010    16         {origin}:(.text)\n000c0000 00000000     0                 {symbol}\n'
            with self.subTest(origin=origin), self.assertRaises(AssertionError):
                proof.check_map(text)

    def test_link_command_same_file_repetition(self):
        argv, archives, objects = proof.link_inputs(COMMAND.replace('libhwui.a', 'libhwui.a libminikin.a'), Path('clang++'), Path.cwd())
        self.assertEqual(archives['libminikin.a'].name, 'libminikin.a')
        self.assertNotIn('python', argv)

    def test_link_collisions_response_and_vm(self):
        for addition in (' other/libminikin.a', ' @hidden.rsp', ' libart_runtime.a', ' libart_runtime.a.evil', ' CMakeFiles/framework_runtime.dir/file.obj'):
            with self.subTest(addition=addition), self.assertRaises(AssertionError):
                proof.link_inputs(COMMAND.replace(' && python', addition+' && python'), Path('clang++'), Path.cwd())
        with self.assertRaises(AssertionError):
            proof.link_inputs(COMMAND+'\n'+COMMAND, Path('clang++'), Path.cwd())

    def test_actual_member_uniqueness(self):
        target=Path('libminikin.a'); other=Path('libother.a'); member='FontCollection.cpp.obj'
        self.assertEqual(proof.require_unique_member({target:[member]}, {}, target, member), member)
        for inventories, objects in (({target:[member,member]}, {}), ({target:[member],other:[member]}, {}),
                                     ({other:[member]}, {}), ({target:[member]}, {member:{Path('direct.obj')}})):
            with self.subTest(inventories=inventories), self.assertRaises(AssertionError):
                proof.require_unique_member(inventories, objects, target, member)


if __name__ == '__main__':
    unittest.main()
