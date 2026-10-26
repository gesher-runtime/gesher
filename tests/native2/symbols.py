#!/usr/bin/env python3
"""Lists what Android native code imports from the platform, for the native2
track's symbol check (tests/native2/native2_test.cpp, ADR 0057).

  symbols.py <llvm-readelf> <out file> <set>=<elf>[,<elf>...] ... [--api <set>=<stub.so>...]

For each set (a program or an app's libraries), every undefined dynamic
symbol that no ELF file of the set defines is written as
"<set> <file> <symbol> <GLOBAL|WEAK>". With --api, every symbol a NDK stub
library (sysroot/usr/lib/<triple>/<api>/libc.so...) defines: the platform's
public API, for a coverage figure.
"""
import os
import subprocess
import sys


def dynsyms(readelf, path):
    out = subprocess.run([readelf, '--dyn-syms', '-W', path], capture_output=True, text=True, check=True).stdout
    defined, undefined = set(), {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 8 or not parts[0].endswith(':'):
            continue
        typ, bind, ndx, name = parts[3], parts[4], parts[6], parts[7]
        name = name.split('@')[0]
        if not name or typ in ('SECTION', 'FILE'):
            continue
        if ndx == 'UND':
            undefined[name] = 'WEAK' if bind == 'WEAK' else 'GLOBAL'
        elif bind in ('GLOBAL', 'WEAK', 'GNU_UNIQUE'):
            defined.add(name)
    return defined, undefined


def main():
    readelf, out_path = sys.argv[1], sys.argv[2]
    lines = []
    api = False
    for arg in sys.argv[3:]:
        if arg == '--api':
            api = True
            continue
        name, files = arg.split('=', 1)
        files = [f for f in files.split(',') if f and os.path.exists(f)]
        if not files:
            print(f'symbols: {name}: no files', file=sys.stderr)
            continue
        if api:
            for f in files:
                defined, _ = dynsyms(readelf, f)
                lines += [f'{name} {os.path.basename(f)} {s} GLOBAL' for s in sorted(defined)]
            continue
        defined, imports = set(), []
        for f in files:
            d, u = dynsyms(readelf, f)
            defined |= d
            imports.append((os.path.basename(f), u))
        for base, u in imports:
            for s, bind in sorted(u.items()):
                if s not in defined:
                    lines.append(f'{name} {base} {s} {bind}')
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    sets = {}
    for l in lines:
        sets[l.split()[0]] = sets.get(l.split()[0], 0) + 1
    for k, v in sets.items():
        print(f'{k}: {v} symbol(s)')


if __name__ == '__main__':
    main()
