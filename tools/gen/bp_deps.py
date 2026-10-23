#!/usr/bin/env python3
"""Walks the dependencies of a Soong Java module, as a survey for Gesher.

  bp_deps.py [--list] MODULE ROOT...

Loads every *.bp file under the ROOTs (AOSP project checkouts), then follows
MODULE's sources and libraries: `srcs`/`device_common_srcs` references,
`static_libs`, `libs`, `defaults`, aconfig groups and libraries, and the
modules Soong generates (`<aidl_interface>-V<n>-java`, `-java-source`,
`<hidl_interface>-java`, sysprop and proto libraries). Prints, per module
type, the modules reached, and the names no loaded file defines (their
project is missing from the ROOTs). With --list, prints every module
reached as `<type> <name> <dir>` instead.
"""
import argparse
import os
import re
import sys
from collections import defaultdict

from bp_files import Parser, tokenize

DEP_PROPS = ('static_libs', 'libs', 'defaults', 'aconfig_declarations',
             'java_aconfig_libraries', 'aconfig_declarations_groups', 'imports')
SRC_PROPS = ('srcs', 'device_common_srcs')


def load(roots):
    modules = {}
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if not d.startswith('.')]
            for f in filenames:
                if not f.endswith('.bp'):
                    continue
                path = os.path.join(dirpath, f)
                try:
                    text = open(path, encoding='utf-8').read()
                    parsed = Parser(tokenize(text, path), path).file()
                except (SystemExit, Exception) as e:  # noqa: BLE001
                    print(f'warning: skipping {path}: {e}', file=sys.stderr)
                    continue
                for mtype, props in parsed:
                    name = props.get('name')
                    if isinstance(name, str):
                        modules[name] = (mtype, props, dirpath)
    return modules


# Names Soong derives from an interface module, mapped back to it.
GENERATED = [
    (re.compile(r'^(.*?)(-V\d+(\.\d+)?)?-java(-source|_static|-constants|-shallow)?$'), None),
    (re.compile(r'^(.*?)-V\d+(\.\d+)?$'), None),
]


def interface_of(modules, name):
    for pattern, _ in GENERATED:
        m = pattern.match(name)
        if m and m.group(1) in modules:
            return m.group(1)
    return None


def as_list(value):
    return [value] if isinstance(value, str) else [v for v in value or [] if isinstance(v, str)]


def walk(modules, start):
    reached, missing = {}, defaultdict(set)
    todo = [(start, None)]
    while todo:
        name, parent = todo.pop()
        name = re.sub(r'\{.*\}$', '', name.lstrip(':'))
        if name in reached:
            continue
        real = name if name in modules else interface_of(modules, name)
        if real is None:
            missing[name].add(parent)
            continue
        mtype, props, base = modules[real]
        reached[name] = (mtype, base)
        if real != name:
            reached[real] = (mtype, base)
        for prop in DEP_PROPS:
            todo += [(dep, name) for dep in as_list(props.get(prop))]
        for prop in SRC_PROPS:
            todo += [(src, name) for src in as_list(props.get(prop)) if src.startswith(':')]
        aidl = props.get('aidl') or {}
        if isinstance(aidl, dict):
            todo += [(dep, name) for dep in as_list(aidl.get('deps'))]
    return reached, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', action='store_true')
    ap.add_argument('module')
    ap.add_argument('roots', nargs='+')
    args = ap.parse_args()
    modules = load(args.roots)
    reached, missing = walk(modules, args.module)
    if args.list:
        for name, (mtype, base) in sorted(reached.items()):
            print(mtype, name, base.replace(os.sep, '/'))
        return
    by_type = defaultdict(list)
    for name, (mtype, _) in reached.items():
        by_type[mtype].append(name)
    print(f'{len(modules)} modules loaded; {len(reached)} reached from {args.module}')
    for mtype, names in sorted(by_type.items(), key=lambda kv: -len(kv[1])):
        print(f'  {mtype}: {len(names)}')
    print(f'{len(missing)} not defined by the loaded files:')
    for name, parents in sorted(missing.items()):
        print(f'  {name}  (from {", ".join(sorted(p or "?" for p in parents)[:3])})')


if __name__ == '__main__':
    main()
