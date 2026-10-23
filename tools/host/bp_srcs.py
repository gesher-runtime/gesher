#!/usr/bin/env python3
"""Prints a module's expanded source files, as CMake lists them.

    bp_srcs.py <aosp dir> <directory of the module's Android.bp> <module>

Reads only that directory's Android.bp (and the filegroups it names in the
same file), evaluating variables and globs; prints absolute paths joined
with ';' for CMake.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'framework'))

from blueprint import Config, parse_file  # noqa: E402
from tree import as_list, glob_regex  # noqa: E402
import re  # noqa: E402


def main():
    aosp, directory, name = sys.argv[1:4]
    base = os.path.join(aosp, directory)
    modules = {p.get('name'): p for _, p in parse_file(os.path.join(base, 'Android.bp'), Config())}

    def expand(mod):
        out = []
        for e in as_list(modules[mod].get('srcs')):
            if e.startswith(':'):
                out += expand(e[1:])
            elif any(c in e for c in '*?['):
                regex = re.compile(glob_regex(e))
                for dp, _, fs in os.walk(base):
                    for f in fs:
                        rel = os.path.relpath(os.path.join(dp, f), base).replace(os.sep, '/')
                        if regex.fullmatch(rel):
                            out.append(os.path.join(base, rel))
            else:
                out.append(os.path.join(base, e))
        return out

    print(';'.join(sorted(dict.fromkeys(expand(name)))), end='')


if __name__ == '__main__':
    main()
