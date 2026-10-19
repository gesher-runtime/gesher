#!/usr/bin/env python3
"""Summarize a `ninja -k 0` log of a target that does not fully compile yet.

    compile-report.py <ninja.log> <sources-cmake-file> <title> <build-dir> > report.md

Prints how many sources compile, the most common errors (with names and paths
normalized so similar errors group together), missing headers, and the list
of failing files. Used to track the ART runtime port in CI.
"""
import collections
import os
import re
import sys


def main():
    log_path, srcs_path, title, build_dir = sys.argv[1:5]
    log = open(log_path, encoding='utf-8', errors='replace').read()
    srcs = re.findall(r'^\s+(\S+\.cc)\s*$', open(srcs_path).read(), re.M)

    failed = sorted(set(
        m.group(1) for m in re.finditer(r'^FAILED: .*?/runtime/(\S+?\.cc)\.obj', log, re.M)))
    errors = re.findall(r'error: (.*)$', log, re.M)
    errors = [e for e in errors if 'too many errors emitted' not in e
              and 'linker command failed' not in e]

    def normalize(e):
        e = re.sub(r"'[^']*' file not found", "'…' file not found", e)
        return re.sub(r"'[^']*'", "'X'", e)

    by_kind = collections.Counter(normalize(e) for e in errors)
    missing = collections.Counter(re.findall(r"'([^']+)' file not found", log))
    undeclared = collections.Counter(re.findall(r"use of undeclared identifier '([^']+)'", log))

    # A file counts as compiled only if its object file exists, so a run that
    # stopped early (or never reached a file) is not reported as success.
    objects = set()
    for dirpath, _, files in os.walk(build_dir):
        for f in files:
            if f.endswith('.cc.obj'):
                path = os.path.join(dirpath, f).replace(os.sep, '/')
                m = re.search(r'/runtime/(.+)\.obj$', path)
                if m:
                    objects.add(m.group(1))
    total = len(srcs)
    ok = sum(1 for s in srcs if s in objects and s not in failed)
    not_attempted = sorted(s for s in srcs if s not in objects and s not in failed)
    out = [f'# {title}', '',
           f'**{ok} / {total}** source files compile ({100 * ok // max(total, 1)}%).', '']
    if by_kind:
        out += ['## Most common errors', '', '| Count | Error |', '|---:|---|']
        out += [f'| {n} | `{k}` |' for k, n in by_kind.most_common(15)]
        out.append('')
    if missing:
        out += ['## Missing headers', '']
        out += [f'- `{h}` ({n})' for h, n in missing.most_common()]
        out.append('')
    if undeclared:
        out += ['## Undeclared identifiers', '']
        out += [f'- `{h}` ({n})' for h, n in undeclared.most_common(25)]
        out.append('')
    if not_attempted:
        out += [f'**{len(not_attempted)}** files were not attempted '
                '(a dependency failed to build).', '']
    if failed:
        out += ['## Failing files', '']
        out += [f'- `{f}`' for f in failed]
        out.append('')
    sys.stdout.write('\n'.join(out))


if __name__ == '__main__':
    main()
