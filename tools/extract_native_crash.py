#!/usr/bin/env python3
"""Extract only technical fatal context and ELF mapping metadata from a log."""
import argparse
import json
from pathlib import Path
import re

MAPPING = re.compile(
    r"^gesher-elf-map: pid=(\d+) abi=(x86_64|arm64-v8a) "
    r"base=0x([0-9a-f]+) bias=0x([0-9a-f]+) span=0x([0-9a-f]+) "
    r"name=([A-Za-z0-9_.+-]{1,128})$"
)
FATAL = re.compile(
    r"Fatal signal (\d+) \(([A-Z0-9]+)\), code (-?\d+) "
    r"\(([A-Z0-9_]+)\),? fault addr (?:0x)?([0-9a-fA-F]+)\b"
)
REGISTER = re.compile(
    r"\b(rax|rbx|rcx|rdx|rsi|rdi|rbp|rsp|r8|r9|r10|r11|r12|r13|r14|r15|rip):"
    r"\s+0x([0-9a-fA-F]{16})\b"
)


def extract(text, pid=None):
    lines = text.splitlines()
    fatals = [(index, match) for index, line in enumerate(lines)
              if (match := FATAL.search(line))]
    mappings = []
    # Mapping records after the fatal marker cannot explain that crash.
    for line in lines[:fatals[-1][0] if fatals else len(lines)]:
        match = MAPPING.fullmatch(line)
        if match:
            process, abi, base, bias, span, name = match.groups()
            mappings.append(dict(pid=int(process), abi=abi, name=name,
                                 base=int(base, 16), bias=int(bias, 16), span=int(span, 16)))
    pids = sorted({item['pid'] for item in mappings})
    if pid is None and len(pids) == 1:
        pid = pids[0]
    result = dict(crash_found=bool(fatals), pid=pid, mapping_pids=pids)
    if not fatals:
        return result
    index, fatal = fatals[-1]
    number, signal, code, code_name, address = fatal.groups()
    result['fatal'] = dict(signal=int(number), signal_name=signal, code=int(code),
                           code_name=code_name, address=f'0x{int(address, 16):x}')
    registers = {}
    host = None
    for line in lines[index + 1:]:
        if 'Fault message:' in line:
            break
        for match in REGISTER.finditer(line):
            registers[match[1]] = f'0x{int(match[2], 16):x}'
        match = re.fullmatch(r'\s*pc module: ([A-Za-z0-9_.+-]{1,128}\.dll)\+0x([0-9a-fA-F]+)', line)
        if match:
            host = dict(name=match[1], offset=f'0x{int(match[2], 16):x}')
    result['registers'] = registers
    result['host_pc'] = host
    result['elf_pc'] = None
    if 'rip' in registers and pid is not None:
        pc = int(registers['rip'], 16)
        for item in reversed(mappings):
            if (item['pid'] == pid and item['base'] <= pc and item['bias'] <= pc
                    and pc - item['base'] < item['span']):
                result['elf_pc'] = dict(name=item['name'], abi=item['abi'],
                                         offset=f"0x{pc - item['bias']:x}",
                                         load_bias=f"0x{item['bias']:x}",
                                         allocation_base=f"0x{item['base']:x}",
                                         span=f"0x{item['span']:x}")
                break
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    # Raw messages, thread names, stack words and fault messages are never
    # copied. This output is metadata, not a native backtrace or cause claim.
    result = extract(args.log.read_text(encoding='utf-8', errors='replace'), args.pid)
    encoded = json.dumps(result, indent=2) + '\n'
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded, encoding='utf-8')
    print(encoded, end='')


if __name__ == '__main__':
    main()
