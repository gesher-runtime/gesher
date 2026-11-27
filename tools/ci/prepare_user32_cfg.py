#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Prepare a DLL-bound, diagnostic-only USER32 initializer instruction map.

The decoder never executes the DLL. Ordinary applications never see this file.
Only x64 PE unwind-described functions, exact instruction bytes and direct
control-flow destinations are accepted. Internal call bodies are stepped over;
no private Windows API signature or result is simulated.
"""
import argparse
import hashlib
import os
import re
import uuid
from pathlib import Path
import struct
import capstone
from capstone import x86_const as x86

VERSION = '5.0.7'
HEADER = struct.Struct('<6I32s2I32s')
ROW = struct.Struct('<2I4B16sI')
LIMIT = 4096
MAGIC = 0x31434655
CONDITIONS = {'jo': 0, 'jno': 1, 'jb': 2, 'jae': 3, 'je': 4, 'jne': 5,
              'jbe': 6, 'ja': 7, 'js': 8, 'jns': 9, 'jp': 10, 'jnp': 11,
              'jl': 12, 'jge': 13, 'jle': 14, 'jg': 15}


class Image:
    def __init__(self, blob):
        self.blob = blob
        assert blob[:2] == b'MZ'
        nt = struct.unpack_from('<I', blob, 60)[0]
        assert blob[nt:nt + 4] == b'PE\0\0'
        machine, sections = struct.unpack_from('<HH', blob, nt + 4)
        optional_size = struct.unpack_from('<H', blob, nt + 20)[0]
        self.optional = nt + 24
        assert machine == 0x8664 and struct.unpack_from('<H', blob, self.optional)[0] == 0x20b
        self.size = struct.unpack_from('<I', blob, self.optional + 56)[0]
        self.entry = struct.unpack_from('<I', blob, self.optional + 16)[0]
        assert 0 < self.entry < self.size <= 32 * 1024 * 1024
        section_table = self.optional + optional_size
        self.sections = [struct.unpack_from('<IIII', blob, section_table + 40 * i + 8)
                         for i in range(sections)]
        pdata, size = struct.unpack_from('<II', blob, self.optional + 112 + 3 * 8)
        assert size % 12 == 0 and size <= len(blob)
        self.functions = [struct.unpack_from('<III', blob, self.offset(pdata) + i)
                          for i in range(0, size, 12)]
        assert all(0 < a < b <= self.size for a, b, _ in self.functions)

    def offset(self, rva):
        matches = [raw + rva - address for virtual, address, size, raw in self.sections
                   if address <= rva < address + size]
        assert len(matches) == 1 and matches[0] < len(self.blob), 'unbacked/ambiguous PE RVA'
        return matches[0]

    def bytes(self, rva, size):
        start = self.offset(rva)
        assert self.offset(rva + size - 1) == start + size - 1
        result = self.blob[start:start + size]
        assert len(result) == size
        return result

    def function(self, rva):
        matches = [(a, b) for a, b, _ in self.functions if a <= rva < b]
        assert len(matches) == 1, 'initializer outside an exact unwind function'
        return matches[0]


def generate(blob):
    assert capstone.__version__ == VERSION, 'unreviewed decoder version'
    image = Image(blob)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    pending = [image.entry]
    visited = set()
    rows = {}
    while pending:
        first, end = image.function(pending.pop())
        if first in visited:
            continue
        visited.add(first)
        assert len(visited) <= 8, 'too many initializer tail functions'
        position = first
        for instruction in decoder.disasm(image.bytes(first, end - first), first):
            assert instruction.address == position
            kind, condition, target = 0, 0, 0
            operands = instruction.operands
            if instruction.group(capstone.CS_GRP_CALL):
                kind = 1
                if len(operands) == 1 and operands[0].type == x86.X86_OP_IMM:
                    target = operands[0].imm
            elif instruction.group(capstone.CS_GRP_JUMP):
                assert len(operands) == 1 and operands[0].type == x86.X86_OP_IMM, 'indirect initializer jump'
                target = operands[0].imm
                if instruction.mnemonic == 'jmp':
                    kind = 3
                else:
                    assert instruction.mnemonic in CONDITIONS, 'unsupported branch condition'
                    kind, condition = 2, CONDITIONS[instruction.mnemonic]
                if not first <= target < end:
                    pending.append(target)
            elif instruction.group(capstone.CS_GRP_RET):
                assert instruction.mnemonic == 'ret'
                kind = 4
            elif instruction.group(capstone.CS_GRP_INT):
                kind = 5
            assert 1 <= instruction.size <= 15 and 0 <= target < image.size
            rows[position] = (position, target, instruction.size, kind, condition, 0,
                              bytes(instruction.bytes).ljust(16, b'\0'), end)
            position += instruction.size
            assert len(rows) <= LIMIT
        assert position == end, 'decoder did not cover complete unwind function'
    assert image.entry in rows
    for rva, target, size, kind, condition, _, _, end in rows.values():
        if kind in (2, 3):
            assert target in rows, 'branch target lacks an exact instruction boundary'
        if kind in (0, 1, 2):
            assert rva + size in rows and rva + size < end, 'missing fallthrough boundary'
    table = b''.join(ROW.pack(*rows[rva]) for rva in sorted(rows))
    header = HEADER.pack(MAGIC, 1, image.size, image.entry, len(rows), 0,
                         hashlib.sha256(blob).digest(), ROW.size, 128, hashlib.sha256(table).digest())
    output = header + table
    verify(output, blob)
    return output


def verify(metadata, blob):
    assert len(metadata) >= HEADER.size
    magic, version, size, entry, count, reserved, digest, row_size, budget, table_digest = HEADER.unpack_from(metadata)
    image = Image(blob)
    assert (magic, version, size, entry, reserved, row_size, budget) == (
        MAGIC, 1, image.size, image.entry, 0, ROW.size, 128)
    assert digest == hashlib.sha256(blob).digest(), 'DLL digest mismatch'
    assert table_digest == hashlib.sha256(metadata[HEADER.size:]).digest(), 'instruction map digest mismatch'
    assert 0 < count <= LIMIT and len(metadata) == HEADER.size + count * ROW.size
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    previous = -1
    known = set()
    records = []
    for i in range(count):
        row = ROW.unpack_from(metadata, HEADER.size + i * ROW.size)
        rva, target, length, kind, condition, reserved, code, end = row
        assert previous < rva < end <= size and 1 <= length <= 15 and kind <= 5 and not reserved
        assert image.function(rva)[1] == end
        assert code[length:] == bytes(16 - length)
        assert code[:length] == image.bytes(rva, length), 'instruction bytes differ from DLL'
        instructions = list(decoder.disasm(code[:length], rva))
        assert len(instructions) == 1 and instructions[0].size == length
        instruction = instructions[0]
        expected = (1 if instruction.group(capstone.CS_GRP_CALL) else
                    3 if instruction.mnemonic == 'jmp' else
                    2 if instruction.group(capstone.CS_GRP_JUMP) else
                    4 if instruction.group(capstone.CS_GRP_RET) else
                    5 if instruction.group(capstone.CS_GRP_INT) else 0)
        assert kind == expected
        assert condition == (CONDITIONS[instruction.mnemonic] if kind == 2 else 0)
        expected_target = (instruction.operands[0].imm if kind in (1, 2, 3) and
                           instruction.operands[0].type == x86.X86_OP_IMM else 0)
        assert target == expected_target
        known.add(rva); previous = rva; records.append(row)
    assert entry in known
    for rva, target, length, kind, _, _, _, end in records:
        if kind in (2, 3): assert target in known
        if kind in (0, 1, 2): assert rva + length in known and rva + length < end


def controls(metadata, blob):
    changes = [(24, 1), (0, 1), (16, 255), (HEADER.size, 1),
               (HEADER.size + 12, 1), (HEADER.size + 10, 1), (HEADER.size + 8, 16)]
    for offset, change in changes:
        changed = bytearray(metadata); changed[offset] ^= change
        if offset >= HEADER.size:
            # The semantic controls also refresh the table digest: byte/hash
            # integrity must not accidentally stand in for decoder validation.
            changed[64:96] = hashlib.sha256(changed[HEADER.size:]).digest()
        try:
            verify(changed, blob)
        except (AssertionError, ValueError, struct.error, KeyError):
            pass
        else:
            raise AssertionError('corrupted metadata accepted: ' + str(offset))
    print('USER32_CFG_INTEGRITY_CONTROLS_PASS cases=' + str(len(changes)))


def bounded_string(image, rva, limit=512):
    value = bytearray()
    for offset in range(limit):
        byte = image.bytes(rva + offset, 1)[0]
        if not byte:
            return value.decode('ascii')
        assert 32 <= byte < 127, 'non-printable PE diagnostic name'
        value.append(byte)
    raise AssertionError('unterminated PE diagnostic name')


def imports(image):
    directory, size = struct.unpack_from('<II', image.blob, image.optional + 112 + 8)
    assert 20 <= size <= 20 * 128 and size % 20 == 0, 'import descriptor bound'
    result = {}
    for offset in range(0, size, 20):
        original, stamp, chain, name, first = struct.unpack('<5I', image.bytes(directory + offset, 20))
        if not any((original, stamp, chain, name, first)):
            return result
        dll = bounded_string(image, name)
        for index in range(8192):
            thunk = struct.unpack('<Q', image.bytes((original or first) + index * 8, 8))[0]
            if not thunk:
                break
            label = '#' + str(thunk & 0xffff) if thunk >> 63 else bounded_string(image, thunk + 2)
            slot = first + index * 8
            assert slot not in result, 'duplicate import slot'
            result[slot] = (dll, label)
        else:
            raise AssertionError('import thunk bound')
    raise AssertionError('missing import descriptor terminator')


def pdb_identity(image):
    blob = image.blob
    debug, size = struct.unpack_from('<II', blob, image.optional + 112 + 6 * 8)
    assert size <= 28 * 64 and size % 28 == 0, 'debug directory bound'
    for at in range(0, size, 28):
        _, _, _, _, kind, length, _, raw = struct.unpack('<IIHHIIII', image.bytes(debug + at, 28))
        if kind == 2:
            assert 24 < length <= 4096 and raw + length <= len(blob), 'CodeView bound'
            data = blob[raw:raw + length]
            if data[:4] == b'RSDS':
                name = data[24:].split(b'\0', 1)[0].decode('ascii').replace('\\', '/').split('/')[-1]
                assert len(name) <= 256 and all(32 <= ord(c) < 127 for c in name)
                return name + ':' + uuid.UUID(bytes_le=data[4:20]).hex.upper() + format(struct.unpack_from('<I', data, 20)[0], 'X')
    return 'unavailable'


def initializer_export(image):
    directory, size = struct.unpack_from('<II', image.blob, image.optional + 112)
    assert size >= 40 and directory + size <= image.size, 'export directory bound'
    fields = struct.unpack('<IIHHIIIIIII', image.bytes(directory, 40))
    count, names_count, functions, names, ordinals = fields[6:11]
    assert 0 < count <= 8192 and names_count <= count, 'export count bound'
    for index in range(names_count):
        name = struct.unpack('<I', image.bytes(names + index * 4, 4))[0]
        if bounded_string(image, name) != 'GdiDllInitialize':
            continue
        ordinal = struct.unpack('<H', image.bytes(ordinals + index * 2, 2))[0]
        assert ordinal < count, 'export ordinal bound'
        target = struct.unpack('<I', image.bytes(functions + ordinal * 4, 4))[0]
        assert 0 < target < image.size
        return ('forwarder:' + bounded_string(image, target) if directory <= target < directory + size
                else hex(target))
    return 'unavailable'


def diagnostic_controls(blob):
    image = Image(blob)
    imports(image)
    pdb_identity(image)
    cases = 0
    for directory, offset, value, verify_function in (
            (1, 4, 1, imports),
            (6, 4, 1, pdb_identity)):
        changed = bytearray(blob)
        struct.pack_into('<I', changed, image.optional + 112 + directory * 8 + offset, value)
        try:
            verify_function(Image(changed))
        except AssertionError:
            cases += 1
        else:
            raise AssertionError('malformed PE diagnostic directory accepted')
    exports, _ = struct.unpack_from('<II', blob, image.optional + 112)
    changed = bytearray(blob)
    struct.pack_into('<I', changed, image.offset(exports) + 20, 0)
    try:
        initializer_export(Image(changed))
    except AssertionError:
        cases += 1
    else:
        raise AssertionError('malformed PE export count accepted')
    directory, _ = struct.unpack_from('<II', blob, image.optional + 112 + 8)
    name = struct.unpack('<I', image.bytes(directory + 12, 4))[0]
    assert image.bytes(name, 1) != b'\0'
    try:
        bounded_string(image, name, 1)
    except AssertionError:
        cases += 1
    else:
        raise AssertionError('unterminated bounded PE name accepted')
    print('USER32_PE_DIAGNOSTIC_CONTROLS_PASS cases=' + str(cases))


def explain(log, blob, metadata, directory):
    verify(metadata, blob)
    image = Image(blob)
    assert log.stat().st_size <= 2 * 1024 * 1024
    text = log.read_text(encoding='utf-8', errors='replace')
    addresses = []
    known = {row[0] for row in ROW.iter_unpack(metadata[HEADER.size:])}
    for number, match in enumerate(re.finditer(r'USER32 CFG: kind=\d+ rva=0x([0-9a-fA-F]{8})', text)):
        assert number < 128, 'CFG log event bound'
        address = int(match[1], 16)
        assert address in known, 'CFG log instruction outside verified metadata'
        if address in addresses: addresses.remove(address)
        addresses.append(address)
    print('USER32_CFG_EXPLAIN dll_sha256=' + hashlib.sha256(blob).hexdigest() + ' pdb=' + pdb_identity(image))
    # Bind the next investigation to these actual local System32 images. Never
    # upload Windows DLLs and never infer that a standalone heap result proves
    # which internal GDI operation failed.
    for name in ('gdi32.dll', 'gdi32full.dll'):
        source = (directory / name).read_bytes()
        assert len(source) <= 32 * 1024 * 1024
        dependency = Image(source)
        print('USER32_CFG_MODULE name=' + name + ' sha256=' + hashlib.sha256(source).hexdigest() +
              ' pdb=' + pdb_identity(dependency) + ' GdiDllInitialize=' + initializer_export(dependency))
    import_slots = imports(image)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    for address in addresses[-12:]:
        instruction = next(decoder.disasm(image.bytes(address, 16), address, count=1), None)
        assert instruction is not None and instruction.address == address
        if instruction.group(capstone.CS_GRP_CALL) and len(instruction.operands) == 1:
            operand = instruction.operands[0]
            if operand.type == x86.X86_OP_MEM and operand.mem.base == x86.X86_REG_RIP and not operand.mem.index:
                slot = instruction.address + instruction.size + operand.mem.disp
                if slot in import_slots:
                    dll, name = import_slots[slot]
                    print('USER32_CFG_IMPORT rva=' + hex(address) + ' iat=' + hex(slot) + ' dll=' + dll + ' name=' + name)
    for address in addresses[-6:]:
        first, end = image.function(address)
        instructions = list(decoder.disasm(image.bytes(first, end - first), first))
        positions = [i for i, instruction in enumerate(instructions) if instruction.address == address]
        assert len(positions) == 1
        index = positions[0]
        print('USER32_CFG_SITE rva=' + hex(address))
        for instruction in instructions[max(0, index - 3):index + 3]:
            print('USER32_CFG_CODE ' + hex(instruction.address) + ' ' + instruction.mnemonic + ' ' + instruction.op_str)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--output', type=Path)
    mode.add_argument('--explain-log', type=Path)
    parser.add_argument('--dll', type=Path, default=Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32/user32.dll')
    parser.add_argument('--selftest', action='store_true')
    parser.add_argument('--metadata', type=Path, default=Path('user32-cfg.bin'))
    args = parser.parse_args()
    assert capstone.__version__ == VERSION, 'unreviewed decoder version'
    blob = args.dll.read_bytes()
    assert len(blob) <= 32 * 1024 * 1024
    if args.explain_log:
        explain(args.explain_log, blob, args.metadata.read_bytes(), args.dll.parent)
        return
    metadata = generate(blob)
    if args.selftest:
        controls(metadata, blob)
        diagnostic_controls(blob)
    args.output.write_bytes(metadata)
    print('USER32_CFG_PREPARED decoder=' + VERSION + ' dll_sha256=' + hashlib.sha256(blob).hexdigest() +
          ' instructions=' + str((len(metadata) - HEADER.size) // ROW.size) + ' event_budget=128')


if __name__ == '__main__': main()
