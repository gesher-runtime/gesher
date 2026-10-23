#!/usr/bin/env python3
"""Resolves <module>+0x<offset> addresses in a Gesher crash report.

  symbolize.py <linker map> [log]   (log defaults to stdin)

The map is the one lld writes with -Wl,-Map (dalvikvm.map next to
dalvikvm.exe). Each address becomes `symbol+0xN` for the nearest preceding
symbol; names are demangled with c++filt when it is on PATH.
"""
import bisect
import re
import shutil
import subprocess
import sys

# lld map lines: "<address> <size> <align> <name>"; symbols are the
# entries with alignment 0 (output and input sections have one). Not by the
# name: C++ names end in ')' (parameter lists) and may contain '/'.
LINE = re.compile(r'^\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+(\S.*)$')


def load_map(path):
    image_base, syms = None, []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LINE.match(line)
        if not m:
            continue
        addr, name = int(m.group(1), 16), m.group(3).strip()
        if name.startswith('__ImageBase'):
            image_base = addr
        if int(m.group(2), 16) != 0 or name.startswith('.') or ':(' in name:
            continue
        syms.append((addr, name))
    syms.sort()
    if image_base is None:
        # lld's COFF map lists RVAs; older formats list absolute addresses.
        image_base = 0 if syms and syms[0][0] < 0x10000000 else 0x140000000
    return image_base, [a for a, _ in syms], [n for _, n in syms]


def main():
    image_base, addrs, names = load_map(sys.argv[1])
    log = open(sys.argv[2], encoding='utf-8', errors='replace') if len(sys.argv) > 2 else sys.stdin
    text = log.read()
    found = {}

    def resolve(m):
        rva = int(m.group(2), 16)
        i = bisect.bisect_right(addrs, image_base + rva) - 1
        if i < 0:
            return m.group(0)
        found[names[i]] = None
        return f'{m.group(0)}  {names[i]}+0x{image_base + rva - addrs[i]:x}'

    out = re.sub(r'(\w[\w.-]*\.exe)\+0x([0-9a-fA-F]+)', resolve, text)
    filt = shutil.which('c++filt') or shutil.which('llvm-cxxfilt')
    if filt and found:
        mangled = list(found)
        demangled = subprocess.run([filt], input='\n'.join(mangled), capture_output=True,
                                   text=True).stdout.split('\n')
        for a, b in zip(mangled, demangled):
            if b and a != b:
                out = out.replace(a, b)
    sys.stdout.write(out)


if __name__ == '__main__':
    main()
