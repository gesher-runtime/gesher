#!/usr/bin/env python3
"""A C/C++ module of an Android.bp, as CMake variables.

    bp_cc.py <aosp dir> <module dir> <module> <prefix> <out.cmake>
             [--targets host,windows] [--arch x86_64] [--exclude REGEX]...

Reads <aosp dir>/<module dir>/Android.bp, applies the module's `defaults`
(those defined in the same file; others are listed in <prefix>_UNRESOLVED),
then the properties of each named `target` (in the order given) and of the
`arch`, as Soong does for one variant. Writes:

  <prefix>_SRCS            sources (absolute; globs expanded; exclude_srcs and
                           --exclude regexes, matched on the relative path, removed)
  <prefix>_CFLAGS          cflags (C and C++)
  <prefix>_CONLYFLAGS      conlyflags
  <prefix>_CPPFLAGS        cppflags
  <prefix>_INCLUDES        local_include_dirs and export_include_dirs (module
                           dir relative) and include_dirs (AOSP relative), absolute
  <prefix>_EXPORT_INCLUDES export_include_dirs, absolute
  <prefix>_UNRESOLVED      defaults from other files, generated sources and
                           libraries, for the record

Filegroups of the same file (`:name` in srcs) are expanded.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'framework'))

from blueprint import Config, parse_file  # noqa: E402
from tree import as_list, glob_regex, merge  # noqa: E402

LIST_PROPS = ('srcs', 'exclude_srcs', 'cflags', 'conlyflags', 'cppflags', 'asflags',
              'local_include_dirs', 'export_include_dirs', 'include_dirs',
              'generated_sources', 'generated_headers', 'static_libs', 'shared_libs',
              'whole_static_libs', 'header_libs')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('aosp')
    ap.add_argument('directory')
    ap.add_argument('module')
    ap.add_argument('prefix')
    ap.add_argument('out')
    ap.add_argument('--targets', default='host,windows')
    ap.add_argument('--arch', default='x86_64')
    ap.add_argument('--exclude', action='append', default=[])
    args = ap.parse_args()

    base = os.path.join(args.aosp, args.directory)
    modules = {}
    for mtype, props in parse_file(os.path.join(base, 'Android.bp'), Config()):
        if 'name' in props:
            modules[props['name']] = (mtype, props)
    if args.module not in modules:
        sys.exit(f'bp_cc.py: no module {args.module} in {args.directory}/Android.bp')
    targets = [t for t in args.targets.split(',') if t]
    unresolved = []

    def variant(props):
        """Folds the chosen target and arch properties into props."""
        out = {k: v for k, v in props.items() if k not in ('target', 'arch', 'multilib')}
        for t in targets:
            branch = (props.get('target') or {}).get(t)
            if isinstance(branch, dict):
                out = merge(out, variant(branch))
        branch = (props.get('arch') or {}).get(args.arch)
        if isinstance(branch, dict):
            out = merge(out, variant(branch))
        branch = (props.get('multilib') or {}).get('lib64')
        if isinstance(branch, dict):
            out = merge(out, variant(branch))
        return out

    def resolved(name):
        _, props = modules[name]
        out = {}
        for d in as_list(props.get('defaults')):
            if d in modules:
                out = merge(out, resolved(d))
            else:
                unresolved.append('defaults:' + d)
        return merge(out, variant({k: v for k, v in props.items() if k != 'defaults'}))

    props = resolved(args.module)

    def expand(entries):
        out = []
        for e in entries:
            if e.startswith(':'):
                name = e[1:].split('{')[0]
                if name in modules and modules[name][0] == 'filegroup':
                    out += expand(as_list(modules[name][1].get('srcs')))
                else:
                    unresolved.append('srcs:' + e)
            elif any(c in e for c in '*?['):
                regex = re.compile(glob_regex(e))
                for dp, _, fs in os.walk(base):
                    for f in fs:
                        rel = os.path.relpath(os.path.join(dp, f), base).replace(os.sep, '/')
                        if regex.fullmatch(rel):
                            out.append(rel)
            else:
                out.append(e)
        return out

    excluded = set(expand(as_list(props.get('exclude_srcs'))))
    patterns = [re.compile(x) for x in args.exclude]
    srcs = [s for s in dict.fromkeys(expand(as_list(props.get('srcs'))))
            if s not in excluded and not any(p.search(s) for p in patterns)]
    # Only sources a C/C++ compiler or assembler takes.
    srcs = [s for s in srcs if re.search(r'\.(c|cc|cpp|cxx|S|s|asm)$', s)]

    def absdir(d, rel_to):
        return os.path.normpath(os.path.join(rel_to, d)).replace(os.sep, '/')

    includes = [absdir(d, base) for d in as_list(props.get('local_include_dirs')) +
                as_list(props.get('export_include_dirs'))]
    includes += [absdir(d, args.aosp) for d in as_list(props.get('include_dirs'))]
    exports = [absdir(d, base) for d in as_list(props.get('export_include_dirs'))]
    for k in ('generated_sources', 'generated_headers'):
        unresolved += [f'{k}:{v}' for v in as_list(props.get(k))]
    for k in ('static_libs', 'shared_libs', 'whole_static_libs', 'header_libs'):
        unresolved += [f'{k}:{v}' for v in as_list(props.get(k))]

    def cmake_list(values):
        return '\n'.join('  "%s"' % v.replace('\\', '/').replace('"', '\\"') for v in values)

    p = args.prefix
    with open(args.out, 'w', encoding='utf-8') as f:
        f.write(f'# Generated by tools/gen/bp_cc.py from {args.directory}/Android.bp, '
                f'module {args.module}, targets {",".join(targets)}, arch {args.arch}.\n')
        f.write(f'set({p}_SRCS\n{cmake_list(absdir(s, base) for s in srcs)})\n')
        for key, prop in (('CFLAGS', 'cflags'), ('CONLYFLAGS', 'conlyflags'),
                          ('CPPFLAGS', 'cppflags')):
            f.write(f'set({p}_{key}\n{cmake_list(as_list(props.get(prop)))})\n')
        f.write(f'set({p}_INCLUDES\n{cmake_list(dict.fromkeys(includes))})\n')
        f.write(f'set({p}_EXPORT_INCLUDES\n{cmake_list(dict.fromkeys(exports))})\n')
        f.write(f'set({p}_UNRESOLVED\n{cmake_list(dict.fromkeys(unresolved))})\n')


if __name__ == '__main__':
    main()
