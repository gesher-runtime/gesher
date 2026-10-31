"""Synthetic in-memory PE metadata ONLY: no binary file or native process."""
import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import sys
import unittest

spec = importlib.util.spec_from_file_location("resource_pe", Path(__file__).with_name("resource-pe-imports.py"))
p = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = p
spec.loader.exec_module(p)

PE = 0x80
OPT = PE + 24
TABLE = OPT + 240
RAW = 0x400
RVA = 0x1000


def put32(value, at, number):
    struct.pack_into("<I", value, at, number)


def directory(value, index, rva, size):
    struct.pack_into("<II", value, OPT + 112 + index * 8, rva, size)


def image(*, regular=True, delay=True, sections=1, section_size=0x1000):
    value = bytearray(RAW + section_size)
    value[:2] = b"MZ"
    put32(value, 60, PE)
    value[PE:PE + 4] = b"PE\0\0"
    struct.pack_into("<HH", value, PE + 4, 0x8664, sections)
    struct.pack_into("<H", value, PE + 20, 240)
    struct.pack_into("<H", value, OPT, 0x20B)
    put32(value, OPT + 56, 0x100000)
    put32(value, OPT + 60, RAW)
    put32(value, OPT + 108, 16)
    struct.pack_into("<IIII", value, TABLE + 8, section_size, RVA, section_size, RAW)
    if regular:
        directory(value, 1, RVA, 40)
        put32(value, RAW + 12, RVA + 0x200)
        value[RAW + 0x200:RAW + 0x200 + 13] = b"KERNEL32.dll\0"
    if delay:
        directory(value, 13, RVA + 0x80, 64)
        struct.pack_into("<II", value, RAW + 0x80, 1, RVA + 0x240)
        value[RAW + 0x240:RAW + 0x240 + 12] = b"dbghelp.DLL\0"
    return value


class Reader(unittest.TestCase):
    def reject(self, value, message=None):
        with self.assertRaises(p.InvalidPE) as caught:
            p.inspect_bytes(bytes(value))
        if message:
            self.assertIn(message, str(caught.exception))

    def test_order_raw_normalized_and_bounds(self):
        result = p.inspect_bytes(bytes(image()))
        self.assertEqual([(d.directory, d.raw_name, d.normalized_name) for d in result["dependencies"]],
                         [("regular", "KERNEL32.dll", "kernel32.dll"), ("delay", "dbghelp.DLL", "dbghelp.dll")])
        self.assertEqual(result["descriptor_slots"], 4)
        self.assertEqual(result["dependencies"][1].delay_attributes, 1)
        self.assertFalse(result["loader_closure"])

    def test_valid_absence_and_single_directories(self):
        for regular, delay, count in ((False, False, 0), (True, False, 1), (False, True, 1)):
            with self.subTest(regular=regular, delay=delay):
                self.assertEqual(len(p.inspect_bytes(bytes(image(regular=regular, delay=delay)))["dependencies"]), count)
        value = image(regular=False, delay=False)
        put32(value, OPT + 108, 0)
        self.assertEqual(p.inspect_bytes(bytes(value))["dependencies"], ())

    def test_actual_input_size_cap_and_type(self):
        self.reject(bytes(p.MAX_BYTES + 1), "size")
        with self.assertRaises(p.InvalidPE):
            p.inspect_bytes(image())  # immutable byte input only

    def test_header_truncations(self):
        value = image()
        for size in (0, 1, 63, PE, PE + 4, PE + 23, OPT + 111, OPT + 239, TABLE + 39, RAW - 1, RAW + 1):
            with self.subTest(size=size):
                self.reject(value[:size])

    def test_wrong_header_facts(self):
        mutations = ((0, b"NZ"), (PE, b"NE\0\0"), (PE + 4, b"\x4c\x01"),
                     (OPT, b"\x0b\x01"), (PE + 6, b"\0\0"), (PE + 6, b"\x61\0"),
                     (PE + 20, b"\x6f\0"))
        for at, replacement in mutations:
            with self.subTest(at=at, replacement=replacement):
                value = image(); value[at:at + len(replacement)] = replacement; self.reject(value)
        for at, number in ((60, 1), (60, 0xFFFFFFF0), (OPT + 108, 17),
                           (OPT + 60, TABLE), (OPT + 60, 0xFFFFFFFF), (OPT + 56, RAW - 1)):
            with self.subTest(at=at, number=number):
                value = image(); put32(value, at, number); self.reject(value)

    def test_section_ambiguity_file_overlap_and_virtual_tail(self):
        for changed in ("rva", "raw", "header", "overflow", "image", "tail", "unbacked", "empty"):
            value = image()
            if changed in ("rva", "raw"):
                struct.pack_into("<H", value, PE + 6, 2)
                struct.pack_into("<IIII", value, TABLE + 48, 0x100,
                                 RVA + (0x100 if changed == "rva" else 0x3000), 0x100,
                                 RAW if changed == "raw" else RAW + 0xF00)
            elif changed == "header": put32(value, TABLE + 12, 0x100)
            elif changed == "overflow": put32(value, TABLE + 12, 0xFFFFFF00)
            elif changed == "image": put32(value, OPT + 56, 0x1800)
            elif changed == "tail":
                put32(value, TABLE + 8, 0x2000); directory(value, 1, RVA + 0x1100, 40)
            elif changed == "unbacked": put32(value, TABLE + 16, 0)
            else:
                put32(value, TABLE + 8, 0); put32(value, TABLE + 16, 0)
            with self.subTest(changed=changed): self.reject(value)

    def test_valid_header_name_mapping_and_raw_larger_than_virtual(self):
        value = image(delay=False)
        put32(value, RAW + 12, 0x300)
        value[0x300:0x308] = b"X.DLL\0\0\0"
        self.assertEqual(p.inspect_bytes(bytes(value))["dependencies"][0].raw_name, "X.DLL")
        put32(value, TABLE + 8, 1)
        self.assertEqual(len(p.inspect_bytes(bytes(value))["dependencies"]), 1)

    def test_directory_pair_range_width_and_termination(self):
        for index, width in ((1, 20), (13, 32)):
            for rva, size in ((0, width), (RVA, 0), (RVA, width - 1),
                              (0xFFFFFFF0, 64), (RVA + 0xFF0, 64)):
                with self.subTest(index=index, rva=rva, size=size):
                    value = image(); directory(value, index, rva, size); self.reject(value)
            value = image()
            directory(value, index, RVA if index == 1 else RVA + 0x80, width)
            self.reject(value, "terminator")

    def test_names_ascii_length_nul_and_mapping(self):
        for text in (b"\0", b"X\x80\0", b"X\x7f\0", b"X\x01\0", b"a" * 129 + b"\0"):
            value = image(delay=False)
            value[RAW + 0x200:RAW + 0x200 + len(text)] = text
            with self.subTest(text=text): self.reject(value)
        value = image(delay=False)
        value[RAW + 0x200:RAW + 0x281] = b"A" * 128 + b"\0"
        self.assertEqual(len(p.inspect_bytes(bytes(value))["dependencies"][0].raw_name), 128)
        for text in (b"space name.dll", b"path/name.dll", b"C:\\name.dll"):
            value = image(delay=False)
            value[RAW + 0x200:RAW + 0x201 + len(text)] = text + b"\0"
            self.assertEqual(p.inspect_bytes(bytes(value))["dependencies"][0].raw_name, text.decode("ascii"))
        for name_rva in (0, 0xFFFFFFFF, RVA + 0xFFF):
            value = image(delay=False); put32(value, RAW + 12, name_rva)
            put32(value, RAW + 16, RVA + 0x300)  # Still a real nonzero descriptor when name=0.
            value[-1] = ord("X")
            with self.subTest(name_rva=name_rva): self.reject(value)

    def test_delay_attributes_not_reinterpreted(self):
        for attributes in (0, 2, 3, 0xFFFFFFFF):
            value = image(regular=False); put32(value, RAW + 0x80, attributes)
            with self.subTest(attributes=attributes): self.reject(value, "delay attributes")

    def test_exact_section_cap(self):
        value = image(regular=False, delay=False)
        value.extend(bytes(0x2000 + 96 * 32 - len(value)))
        struct.pack_into("<H", value, PE + 6, 96)
        put32(value, OPT + 60, 0x2000)
        for index in range(96):
            struct.pack_into("<IIII", value, TABLE + index * 40 + 8,
                             32, 0x10000 + index * 0x1000, 32, 0x2000 + index * 32)
        self.assertEqual(p.inspect_bytes(bytes(value))["dependencies"], ())
        struct.pack_into("<H", value, PE + 6, 97)
        self.reject(value, "section count")

    def test_combined_descriptor_cap_including_sentinels(self):
        value = image(regular=False, delay=False, section_size=0x6000)
        # 510 real imports + regular sentinel + delay sentinel = exactly512.
        directory(value, 1, RVA, 511 * 20)
        directory(value, 13, RVA + 0x3000, 32)
        name = RVA + 0x5000
        value[RAW + 0x5000:RAW + 0x5006] = b"X.dll\0"
        for index in range(510): put32(value, RAW + index * 20 + 12, name)
        result = p.inspect_bytes(bytes(value))
        self.assertEqual((len(result["dependencies"]), result["descriptor_slots"]), (510, 512))
        put32(value, RAW + 510 * 20 + 12, name)
        directory(value, 1, RVA, 512 * 20)
        self.reject(value, "combined descriptor cap")
        value = image(regular=False, delay=False, section_size=0x6000)
        directory(value, 1, RVA, 20)
        directory(value, 13, RVA + 0x100, 511 * 32)
        value[RAW + 0x5000:RAW + 0x5006] = b"X.dll\0"
        for index in range(510):
            struct.pack_into("<II", value, RAW + 0x100 + index * 32, 1, name)
        self.assertEqual(p.inspect_bytes(bytes(value))["descriptor_slots"], 512)
        struct.pack_into("<II", value, RAW + 0x100 + 510 * 32, 1, name)
        directory(value, 13, RVA + 0x100, 512 * 32)
        self.reject(value, "combined descriptor cap")

    def test_stream_three_names_hash_size_and_shape(self):
        value = bytes(image())
        for name in sorted(p.ADMITTED_NAMES):
            entry = {"name": name, "bytes": len(value), "sha256": hashlib.sha256(value).hexdigest()}
            self.assertEqual(len(p.inspect_admitted_stream(name, io.BytesIO(value), entry)["dependencies"]), 2)
        valid = {"name": "cdb.exe", "bytes": len(value), "sha256": hashlib.sha256(value).hexdigest()}
        mutations = ({"name": "dbgeng.dll"}, {"bytes": len(value) - 1}, {"bytes": True},
                     {"bytes": p.MAX_BYTES + 1}, {"sha256": "0" * 64}, {"sha256": "A" * 64},
                     {"sha256": "xyz"}, {"extra": "unadmitted"})
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(p.InvalidPE):
                p.inspect_admitted_stream("cdb.exe", io.BytesIO(value), valid | mutation)
        for name in ("CDB.EXE", "other.dll", "../cdb.exe", "C:/cdb.exe"):
            with self.subTest(name=name), self.assertRaises(p.InvalidPE):
                p.inspect_admitted_stream(name, io.BytesIO(value), valid)
        class Oversize:
            def read(self, count):
                self.count = count
                return bytes(p.MAX_BYTES + 1)
        stream = Oversize()
        with self.assertRaises(p.InvalidPE): p.inspect_admitted_stream("cdb.exe", stream, valid)
        self.assertEqual(stream.count, p.MAX_BYTES + 1)


if __name__ == "__main__":
    unittest.main()
