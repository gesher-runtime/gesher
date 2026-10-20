#!/usr/bin/env python3
"""Expands the source list of a Soong module, as Soong would.

  bp_files.py [-o OUT] [--exclude PATH]... MODULE[.PROPERTY] FILE.bp...

Reads the given Android.bp files (any number, from any projects), finds
MODULE and prints the files of its `srcs` (or PROPERTY, e.g. `java_resources`)
one per line as absolute paths: `:other` references are expanded
recursively, globs (`*`, `**`) are matched against the file system relative
to the .bp file that names them, and `exclude_srcs` is applied. References
to modules that none of the files define are an error.

Only the Blueprint subset that file lists use is supported: modules,
variables (`x = [...]`, `x += [...]`), lists, strings, `+`, and
`select(...)`, which is ignored.
"""
import argparse
import glob
import os
import re
import sys

TOKEN = re.compile(r'''
    (?P<ws>\s+|//[^\n]*|/\*.*?\*/)
  | (?P<str>"(?:[^"\\]|\\.)*"|`[^`]*`)
  | (?P<num>-?\d+)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_.]*)
  | (?P<op>\+=|[{}\[\]():,=+])
''', re.S | re.X)


def tokenize(text, path):
    pos, out = 0, []
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise SystemExit(f'{path}: cannot parse at offset {pos}: {text[pos:pos + 40]!r}')
        pos = m.end()
        kind = m.lastgroup
        if kind == 'ws':
            continue
        val = m.group(kind)
        if kind == 'str':
            val = val[1:-1] if val[0] == '`' else bytes(val[1:-1], 'utf-8').decode('unicode_escape')
        out.append((kind, val))
    return out


class Parser:
    def __init__(self, tokens, path):
        self.t, self.i, self.path, self.vars = tokens, 0, path, {}

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else (None, None)

    def take(self, val=None):
        tok = self.t[self.i]
        if val is not None and tok[1] != val:
            raise SystemExit(f'{self.path}: expected {val!r}, got {tok[1]!r}')
        self.i += 1
        return tok

    def file(self):
        modules = []
        while self.i < len(self.t):
            kind, val = self.take()
            nxt = self.peek()[1]
            if nxt in ('=', '+='):
                self.take()
                v = self.expr()
                self.vars[val] = (self.vars.get(val, []) + v) if nxt == '+=' else v
            elif nxt == '{':
                modules.append((val, self.map()))
            elif nxt == '(':  # e.g. soong_namespace(...) style calls
                self.expr_call()
            else:
                raise SystemExit(f'{self.path}: unexpected {val!r}')
        return modules

    def map(self):
        self.take('{')
        props = {}
        while self.peek()[1] != '}':
            _, key = self.take()
            self.take(':' if self.peek()[1] == ':' else '=')
            props[key] = self.expr()
            if self.peek()[1] == ',':
                self.take()
        self.take('}')
        return props

    def expr_call(self):
        self.take('(')
        depth = 1
        while depth:
            _, v = self.take()
            depth += {'(': 1, ')': -1}.get(v, 0)
        return None

    def value(self):
        kind, val = self.peek()
        if val == '[':
            self.take()
            items = []
            while self.peek()[1] != ']':
                items.append(self.expr())
                if self.peek()[1] == ',':
                    self.take()
            self.take(']')
            return items
        if val == '{':
            return self.map()
        self.take()
        if kind == 'str' or kind == 'num':
            return val
        if val in ('true', 'false'):
            return val == 'true'
        if val == 'select' and self.peek()[1] == '(':
            self.expr_call()
            return None
        if kind == 'ident':
            return self.vars.get(val)
        raise SystemExit(f'{self.path}: unexpected value {val!r}')

    def expr(self):
        v = self.value()
        while self.peek()[1] == '+':
            self.take()
            w = self.value()
            if v is None:
                v = w
            elif w is not None:
                v = v + w
        return v


def load(paths):
    modules = {}
    for path in paths:
        text = open(path, encoding='utf-8').read()
        for mtype, props in Parser(tokenize(text, path), path).file():
            name = props.get('name')
            if isinstance(name, str):
                modules[name] = (mtype, props, os.path.dirname(os.path.abspath(path)))
    return modules


def expand(modules, name, prop, seen=()):
    if name not in modules:
        raise SystemExit(f'module {name!r} is not defined in the given files')
    if name in seen:
        raise SystemExit(f'reference cycle through {name!r}')
    _, props, base = modules[name]

    def resolve(entries):
        files = []
        for e in entries or []:
            if not isinstance(e, str):
                continue
            if e.startswith(':'):
                ref = re.sub(r'\{.*\}$', '', e[1:])
                files += expand(modules, ref, 'srcs', seen + (name,))
            elif any(c in e for c in '*?['):
                files += sorted(glob.glob(os.path.join(base, e), recursive=True))
            else:
                files.append(os.path.join(base, e))
        return [os.path.normpath(f) for f in files if not os.path.isdir(f)]

    files = resolve(props.get(prop))
    excluded = set(resolve(props.get('exclude_srcs'))) if prop == 'srcs' else set()
    return [f for f in files if f not in excluded]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--out')
    ap.add_argument('--exclude', action='append', default=[],
                    help='drop files whose path contains this')
    ap.add_argument('module')
    ap.add_argument('bp', nargs='+')
    args = ap.parse_args()
    name, _, prop = args.module.partition('.')
    files = expand(load(args.bp), name, prop or 'srcs')
    files = [f for f in dict.fromkeys(files) if not any(x in f.replace(os.sep, '/') for x in args.exclude)]
    missing = [f for f in files if not os.path.isfile(f)]
    if missing:
        raise SystemExit(f'{len(missing)} listed file(s) do not exist, e.g. {missing[0]}')
    text = '\n'.join(f.replace(os.sep, '/') for f in files) + '\n'
    if args.out:
        open(args.out, 'w', encoding='utf-8', newline='\n').write(text)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
