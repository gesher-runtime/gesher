#!/usr/bin/env python3
"""Collect a release's aconfig flag values from build/release.

    aconfig_values.py <build/release dir> <release> > <out>
    aconfig_values.py <build/release dir> <release> --aapt2 <res dir|file>...

Follows the release's `inherits` chain in release_configs/, then applies
each release's aconfig/<release>/**/*_flag_values.textproto in order, later
releases overriding earlier ones, as the release config tool does. Writes
`<package>.<name>=true|false` lines, with ` # ro` for read-only flags, which
aconfig_header.py and aconfig_java.py read; with --aapt2, writes instead the
comma-separated list aapt2's --feature-flags takes (`<flag>[:ro]=<value>`)
for the flags the given resources and manifests reference (`featureFlag`),
as the whole list is too long for one argument.
Flags no release sets keep their declared defaults (disabled, read-write).
"""
import os
import re
import sys


def chain(root, release):
    releases = []
    while release:
        releases.append(release)
        path = os.path.join(root, 'release_configs', f'{release}.textproto')
        with open(path) as f:
            m = re.search(r'^inherits:\s*"([^"]+)"', f.read(), re.M)
        release = m.group(1) if m else None
    return releases[::-1]


def values(root, release):
    flags = {}
    for rel in chain(root, release):
        top = os.path.join(root, 'aconfig', rel)
        for dirpath, dirnames, filenames in os.walk(top):
            dirnames.sort()
            for name in sorted(filenames):
                if not name.endswith('.textproto'):
                    continue
                with open(os.path.join(dirpath, name)) as f:
                    text = f.read()
                for block in re.findall(r'flag_value\s*{([^}]*)}', text):
                    field = dict(re.findall(r'(\w+):\s*"?([\w.]+)"?', block))
                    key = f'{field["package"]}.{field["name"]}'
                    flags[key] = (field.get('state') == 'ENABLED',
                                  field.get('permission') == 'READ_ONLY')
    return flags


def referenced(paths):
    names = set()
    for path in paths:
        files = [path] if os.path.isfile(path) else (
            os.path.join(d, f) for d, _, fs in os.walk(path) for f in fs
            if f.endswith('.xml'))
        for name in files:
            with open(name, encoding='utf-8', errors='replace') as f:
                names.update(re.findall(
                    r'featureFlag\s*=\s*"\s*!?\s*([\w.]+)\s*"', f.read()))
    return names


def main():
    root, release = sys.argv[1], sys.argv[2]
    flags = values(root, release)
    if sys.argv[3:4] == ['--aapt2']:
        print(','.join(
            f'{k}{":ro" if ro else ""}={"true" if on else "false"}'
            for k in sorted(referenced(sys.argv[4:]))
            for on, ro in [flags.get(k, (False, False))]))
        return
    print(f'# aconfig flag values of release {release}, from build/release '
          '(tools/gen/aconfig_values.py).')
    for k, (on, ro) in sorted(flags.items()):
        print(f'{k}={"true" if on else "false"}{" # ro" if ro else ""}')


if __name__ == '__main__':
    main()
