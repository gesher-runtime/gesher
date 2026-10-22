#!/usr/bin/env python3
"""Java stubs from API signature files, for compiling against a library.

    api_stubs.py <out dir> <api.txt>... [--known <api.txt>...]

Reads metalava signature files (format 2.0: current.txt, system-current.txt,
module-lib-current.txt; several files of one library merge) and writes a
compilable Java source per top-level class whose methods throw, the way
java_api_library builds stubs from text. Constants keep their values, so
code compiled against the stubs can switch on them. --known files describe
classes the stubs extend but do not define (the framework's, libcore's), for
their constructors.
"""
import os
import re
import sys
from collections import OrderedDict


def strip_annotations(text):
    """Removes @Annotation and @Annotation(...) (with nested parentheses
    and strings) from a declaration."""
    out, i = [], 0
    while i < len(text):
        if text[i] == '@' and not text.startswith('@interface', i):
            j = i + 1
            while j < len(text) and (text[j].isalnum() or text[j] in '._'):
                j += 1
            if j < len(text) and text[j] == '(':
                depth, k = 0, j
                while k < len(text):
                    c = text[k]
                    if c == '"':
                        k += 1
                        while text[k] != '"':
                            k += 2 if text[k] == '\\' else 1
                    elif c == '(':
                        depth += 1
                    elif c == ')':
                        depth -= 1
                        if depth == 0:
                            break
                    k += 1
                j = k + 1
            i = j
            while i < len(text) and text[i] == ' ':
                i += 1
            continue
        if text[i] == '"':
            j = i + 1
            while text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        out.append(text[i])
        i += 1
    return re.sub(r'\s+', ' ', ''.join(out)).strip()


def split_top(text, sep=','):
    parts, depth, cur = [], 0, []
    for c in text:
        if c in '<(':
            depth += 1
        elif c in '>)':
            depth -= 1
        if c == sep and depth == 0:
            parts.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
    if ''.join(cur).strip():
        parts.append(''.join(cur).strip())
    return parts


MODIFIERS = {'public', 'protected', 'private', 'static', 'final', 'abstract', 'default',
             'sealed', 'synchronized', 'native', 'transient', 'volatile', 'strictfp',
             'deprecated', 'open', 'non-sealed'}


class Member:
    def __init__(self, kind, mods, text):
        self.kind, self.mods, self.text = kind, mods, text


class Cls:
    def __init__(self, package, name, kind, mods, header):
        self.package, self.name, self.kind, self.mods = package, name, kind, mods
        self.header = header  # type params and supertypes, as written
        self.members = []
        self.nested = OrderedDict()

    @property
    def simple(self):
        return self.name.rsplit('.', 1)[-1]

    def type_params(self):
        m = re.match(r'^<(.*)>', self.header)
        return m.group(0) if m else ''

    def superclass(self):
        m = re.search(r'\bextends\s+([\w.]+)', self.header)
        return m.group(1) if m and self.kind == 'class' else None


def parse(path, classes):
    package, cls = None, None
    with open(path, encoding='utf-8') as f:
        for raw in f:
            line = raw.rstrip('\n')
            s = line.strip()
            if not s or s.startswith('//'):
                continue
            m = re.match(r'^package (\S+) \{$', s)
            if m:
                package = m.group(1)
                continue
            if s == '}':
                cls = None
                continue
            if cls is None:
                decl = strip_annotations(s.rstrip('{').strip())
                m = re.match(r'^((?:[\w-]+ )*)(class|interface|enum|@interface) ([\w.]+)(.*)$', decl)
                if not m:
                    raise SystemExit(f'{path}: cannot parse class line: {s}')
                mods = m.group(1).split()
                name = m.group(3)
                key = f'{package}.{name}'
                cls = classes.get(key)
                if cls is None:
                    cls = classes[key] = Cls(package, name, m.group(2), mods,
                                             normalize_header(m.group(4).strip()))
                continue
            m = re.match(r'^(ctor|method|field|enum_constant|property) (.*);$', s)
            if not m or m.group(1) == 'property':
                continue
            text = strip_annotations(m.group(2))
            words = []
            rest = text
            while True:
                w = rest.split(' ', 1)
                if len(w) == 2 and w[0] in MODIFIERS:
                    words.append(w[0])
                    rest = w[1]
                else:
                    break
            member = Member(m.group(1), words, rest)
            if all(member.text != x.text or member.kind != x.kind for x in cls.members):
                cls.members.append(member)


def normalize_header(header):
    """Type parameters, then `extends`/`implements` lists with commas
    (signature files separate the types with spaces)."""
    words, depth, cur = [], 0, []
    for c in header + ' ':
        if c == '<':
            depth += 1
        elif c == '>':
            depth -= 1
        if c == ' ' and depth == 0:
            if cur:
                words.append(''.join(cur))
                cur = []
        else:
            cur.append(c)
    out, clause = [], []
    for w in words:
        if w in ('extends', 'implements'):
            if clause:
                out.append(', '.join(clause))
                clause = []
            out.append(w)
        elif out and out[-1] in ('extends', 'implements') or clause:
            clause.append(w)
        else:
            out.append(w)
    if clause:
        out.append(', '.join(clause))
    return ' '.join(out)


STUB = 'throw new RuntimeException("Stub!");'


def params_decl(params):
    out = []
    for i, p in enumerate(split_top(params)):
        p = p.strip()
        # Format 2.0 omits names; newer formats may carry one.
        if re.match(r'^.*[\w>\]](\.\.\.)?\s+\w+$', p) and not p.endswith('...'):
            ptype = p.rsplit(' ', 1)[0]
        else:
            ptype = p
        out.append(f'{ptype} p{i}')
    return ', '.join(out)


def default_value(ptype):
    ptype = ptype.strip()
    prims = {'boolean': 'false', 'char': "'\\0'", 'byte': '(byte) 0', 'short': '(short) 0',
             'int': '0', 'long': '0L', 'float': '0f', 'double': '0d'}
    if ptype in prims:
        return prims[ptype]
    base = re.sub(r'<.*>', '', ptype).replace('...', '[]')
    return f'({base}) null'


def split_call(text):
    """`<T> T name(params) throws X default Y` -> parts."""
    m = re.match(r'^(<.*?>\s+)?(.*?)\s*(\w+)\((.*)\)(\s+throws\s+[^=]*?)?(\s+default\s+.*)?$', text)
    return m


def emit_class(cls, known, indent, outer_kind=None):
    pad = ' ' * indent
    mods = [m for m in cls.mods if m not in ('deprecated', 'sealed', 'non-sealed', 'open')]
    if cls.kind in ('interface', '@interface'):
        mods = [m for m in mods if m != 'abstract']
    if indent == 0:
        mods = [m for m in mods if m != 'static']
    header = cls.header
    lines = [f'{pad}{" ".join(mods + [cls.kind])} {cls.simple}{(" " + header) if header else ""} {{']
    body = pad + '    '
    if cls.kind == 'enum':
        consts = [m.text.rsplit(' ', 1)[-1] for m in cls.members if m.kind == 'enum_constant']
        lines.append(f'{body}{", ".join(consts)};')
    for m in cls.members:
        if m.kind == 'field':
            mm = re.match(r'^(.*?) (\w+)(?: = (.*))?$', m.text)
            ftype, fname, value = mm.groups()
            fmods = [x for x in m.mods if x != 'deprecated']
            if value is not None:
                lines.append(f'{body}{" ".join(fmods)} {ftype} {fname} = {value};')
            elif 'final' in fmods:
                init = default_value(ftype)
                if 'static' in fmods:
                    lines.append(f'{body}{" ".join(fmods)} {ftype} {fname} = {stub_value(ftype)};')
                else:
                    lines.append(f'{body}{" ".join(fmods)} {ftype} {fname} = {init};')
            else:
                lines.append(f'{body}{" ".join(fmods)} {ftype} {fname};')
        elif m.kind == 'ctor':
            mm = split_call(m.text)
            if not mm:
                continue
            tparams, _, _, params, throws, _ = mm.groups()
            cmods = [x for x in m.mods if x != 'deprecated']
            sup = super_call(cls, known)
            lines.append(f'{body}{" ".join(cmods)} {tparams or ""}{cls.simple}({params_decl(params)})'
                         f'{throws or ""} {{ {sup}{STUB} }}')
        elif m.kind == 'method':
            mm = split_call(m.text)
            if not mm:
                continue
            tparams, ret, name, params, throws, default = mm.groups()
            if cls.kind == 'enum' and name in ('values', 'valueOf'):
                continue
            mmods = [x for x in m.mods if x not in ('deprecated',)]
            if cls.kind == '@interface':
                lines.append(f'{body}{ret} {name}(){default or ""};')
                continue
            abstract = 'abstract' in mmods
            if cls.kind == 'interface':
                mmods = [x for x in mmods if x != 'abstract']
                abstract = not ({'default', 'static', 'private'} & set(mmods))
            decl = f'{" ".join(mmods)} {tparams or ""}{ret} {name}({params_decl(params)}){throws or ""}'
            lines.append(f'{body}{decl.strip()}' + (';' if abstract or 'native' in mmods else f' {{ {STUB} }}'))
    for nested in cls.nested.values():
        lines += emit_class(nested, known, indent + 4, cls.kind)
    # A constructor for subclasses among the stubs, which call it.
    if cls.kind == 'class':
        sup = super_call(cls, known)
        lines.append(f'{body}{cls.simple}(gesher.stubs.Marker m) {{ {sup}{STUB} }}')
    lines.append(f'{pad}}}')
    return lines


def stub_value(ftype):
    t = ftype.strip()
    if t in ('boolean',):
        return 'Boolean.parseBoolean("false")'
    if t in ('int', 'long', 'short', 'byte', 'char', 'float', 'double'):
        return f'({t}) Integer.parseInt("0")'
    return 'null'


def super_call(cls, known):
    sup = cls.superclass()
    if sup is None:
        return ''
    target = known.get(sup) or known.get(resolve_lang(sup))
    if target is not None and target.get('stub'):
        return 'super((gesher.stubs.Marker) null); '
    ctors = target.get('ctors') if target else None
    if not ctors or [] in ctors:
        return ''
    args = ', '.join(default_value(p) for p in ctors[0])
    return f'super({args}); '


def resolve_lang(name):
    return name if '.' in name else f'java.lang.{name}'


def knowledge(classes, stub_keys):
    known = {}
    for key, cls in classes.items():
        ctors = []
        for m in cls.members:
            if m.kind == 'ctor':
                mm = split_call(m.text)
                if mm:
                    ctors.append([p for p in split_top(mm.group(4))])
        entry = {'ctors': ctors, 'stub': key in stub_keys}
        known[key] = entry
        known[key.replace('.', '.')] = entry
    return known


def main():
    args = sys.argv[1:]
    out_dir = args.pop(0)
    known_files = []
    if '--known' in args:
        i = args.index('--known')
        known_files = args[i + 1:]
        args = args[:i]
    classes = OrderedDict()
    for path in args:
        parse(path, classes)
    stub_keys = set(classes)
    all_classes = OrderedDict(classes)
    for path in known_files:
        parse(path, all_classes)
    known = knowledge(all_classes, stub_keys)
    # Nest inner classes in their outer classes.
    tops = OrderedDict()
    for key, cls in classes.items():
        parts = cls.name.split('.')
        if len(parts) == 1:
            tops.setdefault(key, cls)
            continue
        outer_key = f'{cls.package}.{parts[0]}'
        outer = tops.get(outer_key) or classes.get(outer_key)
        if outer is None:
            outer = Cls(cls.package, parts[0], 'class', ['public'], '')
            classes[outer_key] = outer
        tops.setdefault(outer_key, outer)
        node = outer
        for depth in range(1, len(parts) - 1):
            name = '.'.join(parts[:depth + 1])
            child = classes.get(f'{cls.package}.{name}')
            if child is None:
                child = Cls(cls.package, name, 'class', ['public', 'static'], '')
                classes[f'{cls.package}.{name}'] = child
            node.nested.setdefault(name, child)
            node = child
        node.nested[cls.name] = cls
    count = 0
    for key, cls in tops.items():
        path = os.path.join(out_dir, *cls.package.split('.'), f'{cls.simple}.java')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(f'// Generated by gesher tools/framework/api_stubs.py. Do not edit.\n'
                    f'package {cls.package};\n\n')
            f.write('\n'.join(emit_class(cls, known, 0)) + '\n')
        count += 1
    marker = os.path.join(out_dir, 'gesher', 'stubs', 'Marker.java')
    os.makedirs(os.path.dirname(marker), exist_ok=True)
    with open(marker, 'w', newline='\n') as f:
        f.write('package gesher.stubs;\n\n/** Selects the stubs\' own constructors. */\n'
                'public final class Marker { private Marker() {} }\n')
    print(f'{count} stub classes from {len(args)} API files')


if __name__ == '__main__':
    main()
