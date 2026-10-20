#!/usr/bin/env python3
"""Lists symbols of COFF object files (no binutils needed).

  coff-symbols.py [--undefined|--defined] FILE.obj...

Prints one symbol per line. Used to find the C++ functions that ART's
assembly calls, which must be declared sysv_abi (docs/decisions/0006).
"""
import struct
import sys


def symbols(path):
    data = open(path, 'rb').read()
    machine, nsections, _, symtab, nsyms = struct.unpack_from('<HHIII', data, 0)
    if machine != 0x8664:
        raise SystemExit(f'{path}: not an x86-64 COFF object')
    strtab = symtab + 18 * nsyms
    i = 0
    while i < nsyms:
        off = symtab + 18 * i
        raw = data[off:off + 8]
        value, section, typ, storage, naux = struct.unpack_from('<IhHBB', data, off + 8)
        if raw[:4] == b'\0\0\0\0':
            (name_off,) = struct.unpack_from('<I', raw, 4)
            end = data.index(b'\0', strtab + name_off)
            name = data[strtab + name_off:end].decode()
        else:
            name = raw.rstrip(b'\0').decode()
        if storage == 2:  # IMAGE_SYM_CLASS_EXTERNAL
            yield name, section != 0 or value != 0
        i += 1 + naux


def main(argv):
    mode = 'all'
    if argv and argv[0] in ('--undefined', '--defined'):
        mode, argv = argv[0][2:], argv[1:]
    out = set()
    for path in argv:
        for name, defined in symbols(path):
            if mode == 'all' or (mode == 'defined') == defined:
                out.add(name)
    print('\n'.join(sorted(out)))


if __name__ == '__main__':
    main(sys.argv[1:])
