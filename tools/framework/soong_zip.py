#!/usr/bin/env python3
"""The part of Soong's soong_zip that genrules use to make source jars.

    soong_zip.py [-jar|-srcjar] [-write_if_changed] -o <out> (-C <dir> -D <dir> | -f <file>)...

Zips the files under each -D directory, stored under their paths relative
to the preceding -C directory, in sorted order with fixed timestamps.
"""
import os
import sys
import zipfile


def main():
    args = sys.argv[1:]
    out, base, entries = None, None, []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ('-jar', '-srcjar', '-write_if_changed', '-symlinks=false', '-d'):
            i += 1
            continue
        if a == '-o':
            out = args[i + 1]
        elif a == '-C':
            base = args[i + 1]
        elif a == '-D':
            root = args[i + 1]
            for dp, _, fs in os.walk(root):
                for f in fs:
                    path = os.path.join(dp, f)
                    entries.append((os.path.relpath(path, base or root), path))
        elif a == '-f':
            path = args[i + 1]
            entries.append((os.path.relpath(path, base) if base else os.path.basename(path), path))
        else:
            raise SystemExit(f'soong_zip.py: unsupported argument {a}')
        i += 2
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, path in sorted(entries):
            info = zipfile.ZipInfo(name.replace(os.sep, '/'), (2008, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, 'rb') as f:
                z.writestr(info, f.read())


if __name__ == '__main__':
    main()
