#!/usr/bin/env python3
"""Summarize a `ninja -k 0` log of a target that does not fully compile yet.

    compile-report.py <ninja.log> <sources-cmake-file> <title> > report.md

Prints how many sources compile, the most common errors (with names and paths
normalized so similar errors group together), missing headers, and the list
of failing files. Used to track the ART runtime port in CI.
"""
import collections
import re
import sys


def main():
    log_path, srcs_path, title = sys.argv[1:4]
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

    total = len(srcs)
    ok = total - len(failed)
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
    if failed:
        out += ['## Failing files', '']
        out += [f'- `{f}`' for f in failed]
        out.append('')
    sys.stdout.write('\n'.join(out))


if __name__ == '__main__':
    main()
