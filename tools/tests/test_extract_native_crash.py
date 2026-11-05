import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from extract_native_crash import extract


def mapping(pid=42, name='fixture.so'):
    return f'gesher-elf-map: pid={pid} abi=x86_64 base=0x10000 bias=0xf000 span=0x2000 name={name}\n'


FATAL = ('Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR) fault addr 0000000000000308\n'
         '  rax: 0x0000000000000000  rip: 0x0000000000010123\n'
         '  rsp: 0x0000000000500000\n')


class ExtractNativeCrashTest(unittest.TestCase):
    def test_pc_uses_load_bias_and_excludes_payload(self):
        text = ('private authentication payload\n' + mapping() + FATAL +
                'Thread: 9 "private thread name"\nBacktrace:\n'
                'Fault message: private fault text rip: 0x0000000000010999\n')
        result = extract(text)
        self.assertEqual(result['elf_pc']['offset'], '0x1123')
        self.assertEqual(result['elf_pc']['name'], 'fixture.so')
        self.assertEqual(result['fatal']['address'], '0x308')
        self.assertNotIn('private', json.dumps(result))
        self.assertEqual(result['registers']['rip'], '0x10123')

    def test_multiple_processes_require_selection(self):
        text = mapping(1, 'first.so') + mapping(2, 'second.so') + FATAL
        self.assertIsNone(extract(text)['elf_pc'])
        self.assertEqual(extract(text, 1)['elf_pc']['name'], 'first.so')
        self.assertEqual(extract(text, 2)['elf_pc']['name'], 'second.so')

    def test_latest_fatal_does_not_inherit_old_registers(self):
        text = mapping() + FATAL + 'Fault message:\n' + FATAL.splitlines()[0]
        result = extract(text)
        self.assertEqual(result['registers'], {})
        self.assertIsNone(result['elf_pc'])

    def test_invalid_mapping_and_outside_address_are_rejected(self):
        for text in (mapping(name='bad name.so') + FATAL,
                     mapping() + FATAL.replace('0000000000010123', '0000000000012000'),
                     FATAL + mapping()):
            self.assertIsNone(extract(text, 42)['elf_pc'])

    def test_no_fatal_means_no_register_extraction(self):
        result = extract(mapping() + 'auth: 0x0000000000000001\n')
        self.assertFalse(result['crash_found'])
        self.assertNotIn('registers', result)


if __name__ == '__main__':
    unittest.main()
