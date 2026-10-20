#!/usr/bin/env python3
"""Sort compiled classes into jars by the source file each came from.

  split_classes.py <classes dir> <out dir> NAME=<source list>...

Each source list holds one .java path per line (bp_files.py output). Every
.class file under <classes dir> whose SourceFile attribute and package match
a listed source is copied to <out dir>/NAME/; classes matching no list (for
example ICU classes javac compiled because libcore references them) are left
out. Prints how many classes went where.
"""
import os
import shutil
import struct
import sys


def source_file(data):
    """The SourceFile attribute of a class file, or None."""
    count = struct.unpack_from('>H', data, 8)[0]
    pool, i, idx = {}, 10, 1
    while idx < count:
        tag = data[i]
        if tag == 1:  # Utf8
            n = struct.unpack_from('>H', data, i + 1)[0]
            pool[idx] = data[i + 3:i + 3 + n].decode('utf-8', 'replace')
            i += 3 + n
        elif tag in (3, 4):
            i += 5
        elif tag in (5, 6):
            i += 9
            idx += 1
        elif tag in (7, 8, 16, 19, 20):
            i += 3
        elif tag in (9, 10, 11, 12, 17, 18):
            i += 5
        elif tag == 15:
            i += 4
        else:
            raise ValueError(f'constant pool tag {tag}')
        idx += 1
    i += 6  # access, this, super
    n = struct.unpack_from('>H', data, i)[0]
    i += 2 + 2 * n
    for _ in range(2):  # fields, methods
        n = struct.unpack_from('>H', data, i)[0]
        i += 2
        for _ in range(n):
            i += 6
            a = struct.unpack_from('>H', data, i)[0]
            i += 2
            for _ in range(a):
                i += 6 + struct.unpack_from('>I', data, i + 2)[0]
    n = struct.unpack_from('>H', data, i)[0]
    i += 2
    for _ in range(n):
        name, length = struct.unpack_from('>HI', data, i)
        if pool.get(name) == 'SourceFile':
            return pool[struct.unpack_from('>H', data, i + 6)[0]]
        i += 6 + length
    return None


def main():
    classes, out = sys.argv[1], sys.argv[2]
    owner = {}  # (package dir, file name) -> jar
    for arg in sys.argv[3:]:
        name, path = arg.split('=', 1)
        for line in open(path, encoding='utf-8'):
            src = line.strip()
            if not src.endswith('.java'):
                continue
            # The package comes from the class file location, so match on
            # the trailing package path + file name.
            owner.setdefault(os.path.basename(src), []).append((src, name))
    counts = {}
    for dirpath, _, files in os.walk(classes):
        for f in files:
            if not f.endswith('.class') or f == 'module-info.class':
                continue
            full = os.path.join(dirpath, f)
            sf = source_file(open(full, 'rb').read())
            pkg = os.path.relpath(dirpath, classes).replace(os.sep, '/')
            jar = None
            for src, name in owner.get(sf, []):
                if src.endswith('/' + pkg + '/' + sf):
                    jar = name
                    break
            counts[jar] = counts.get(jar, 0) + 1
            if jar:
                dst = os.path.join(out, jar, os.path.relpath(full, classes))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(full, dst)
    for jar, n in sorted(counts.items(), key=lambda kv: str(kv[0])):
        print(f'{jar or "(not in any jar)"}: {n} classes')


if __name__ == '__main__':
    main()
