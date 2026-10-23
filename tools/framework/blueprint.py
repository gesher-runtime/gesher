"""Blueprint (Android.bp) files, as far as Gesher's Java builds need them.

Parses modules, variables, lists, maps, strings, booleans, numbers, `+`,
and evaluates `select(...)` and `soong_config_variables` against a release's
build flags (build/release: flag_declarations defaults, then the release
chain's flag_values), as Soong does for a device build of that release.
"""
import os
import re

TOKEN = re.compile(r'''
    (?P<ws>\s+|//[^\n]*|/\*.*?\*/)
  | (?P<str>"(?:[^"\\]|\\.)*"|`[^`]*`)
  | (?P<num>-?\d+)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_.]*)
  | (?P<op>\+=|[{}\[\]():,=+@])
''', re.S | re.X)


class BlueprintError(Exception):
    pass


def tokenize(text, path):
    pos, out = 0, []
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise BlueprintError(f'{path}: cannot parse at offset {pos}: {text[pos:pos + 40]!r}')
        pos = m.end()
        kind = m.lastgroup
        if kind == 'ws':
            continue
        val = m.group(kind)
        if kind == 'str':
            val = val[1:-1] if val[0] == '`' else bytes(val[1:-1], 'utf-8').decode('unicode_escape')
        out.append((kind, val))
    return out


class Unset:
    """`unset` in a select branch: the property keeps no value."""


class Config:
    """Build flags of one release, for select() and soong config variables."""

    def __init__(self, release_dir=None, release=None, overrides=None):
        self.flags = {}
        if release_dir:
            self._load(release_dir, release)
        self.flags.update(overrides or {})

    @staticmethod
    def _value(text):
        m = re.search(r'value\s*:?\s*{\s*(\w+)_value\s*:\s*("(?:[^"\\]|\\.)*"|\w+)', text)
        if not m:
            return None
        kind, raw = m.groups()
        if kind == 'bool':
            return raw == 'true'
        if kind == 'string':
            return raw[1:-1]
        return raw

    def _load(self, root, release):
        decl = os.path.join(root, 'flag_declarations')
        for name in os.listdir(decl) if os.path.isdir(decl) else []:
            if name.endswith('.textproto'):
                with open(os.path.join(decl, name)) as f:
                    text = f.read()
                m = re.search(r'name:\s*"([^"]+)"', text)
                if m:
                    self.flags[m.group(1)] = self._value(text)
        chain = []
        while release:
            chain.append(release)
            path = os.path.join(root, 'release_configs', f'{release}.textproto')
            with open(path) as f:
                m = re.search(r'^inherits:\s*"([^"]+)"', f.read(), re.M)
            release = m.group(1) if m else None
        for rel in reversed(chain):
            vals = os.path.join(root, 'flag_values', rel)
            for name in sorted(os.listdir(vals)) if os.path.isdir(vals) else []:
                if name.endswith('.textproto'):
                    with open(os.path.join(vals, name)) as f:
                        text = f.read()
                    m = re.search(r'name:\s*"([^"]+)"', text)
                    if m:
                        self.flags[m.group(1)] = self._value(text)

    def release_flag(self, name):
        return self.flags.get(name)

    def soong_config(self, namespace, variable):
        # Soong config variables Gesher meets are release flags by another
        # name (bootclasspath.release_ranging_stack is RELEASE_RANGING_STACK).
        return self.flags.get(variable.upper())


class Parser:
    def __init__(self, tokens, path, config):
        self.t, self.i, self.path, self.config, self.vars = tokens, 0, path, config, {}

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else (None, None)

    def take(self, val=None):
        if self.i >= len(self.t):
            raise BlueprintError(f'{self.path}: unexpected end of file')
        tok = self.t[self.i]
        if val is not None and tok[1] != val:
            raise BlueprintError(f'{self.path}: expected {val!r}, got {tok[1]!r}')
        self.i += 1
        return tok

    def file(self):
        modules = []
        while self.i < len(self.t):
            _, val = self.take()
            nxt = self.peek()[1]
            if nxt in ('=', '+='):
                self.take()
                v = self.expr()
                self.vars[val] = add(self.vars.get(val), v) if nxt == '+=' else v
            elif nxt == '{':
                modules.append((val, self.map()))
            else:
                raise BlueprintError(f'{self.path}: unexpected {val!r}')
        return modules

    def map(self):
        self.take('{')
        props = {}
        while self.peek()[1] != '}':
            _, key = self.take()
            self.take(':' if self.peek()[1] == ':' else '=')
            value = self.expr()
            if value is not Unset:
                props[key] = value
            if self.peek()[1] == ',':
                self.take()
        self.take('}')
        return props

    def condition(self, toks):
        """The value a select() condition has in the configuration."""
        kind = toks[0][1]
        names = [v for k, v in toks[1:] if k == 'str']
        if kind == 'release_flag':
            return self.config.release_flag(names[0])
        if kind == 'soong_config_variable':
            return self.config.soong_config(*names[:2])
        return None  # arch, os, product_variable, ...: the default branch

    def select(self):
        self.take('select')
        self.take('(')
        depth = 1
        # Conditions: one call, or a tuple of calls; then the cases map.
        cond_toks = []
        while not (depth == 1 and self.peek()[1] == ','):
            tok = self.take()
            if tok == ('op', '('):
                depth += 1
            elif tok == ('op', ')'):
                depth -= 1
            cond_toks.append(tok)
        self.take(',')
        if cond_toks and cond_toks[0] == ('op', '('):
            parts, cur, d = [], [], 0
            for tok in cond_toks[1:-1]:
                if tok == ('op', '('):
                    d += 1
                elif tok == ('op', ')'):
                    d -= 1
                if tok == ('op', ',') and d == 0:
                    parts.append(cur)
                    cur = []
                else:
                    cur.append(tok)
            parts.append(cur)
            values = tuple(self.condition(p) for p in parts if p)
        else:
            values = (self.condition(cond_toks),)
        self.take('{')
        chosen, found = None, False
        while self.peek()[1] != '}':
            pattern = self.pattern()
            self.take(':')
            value = self.expr()
            if not found and self.matches(pattern, values):
                chosen, found = value, True
            if self.peek()[1] == ',':
                self.take()
        self.take('}')
        if self.peek()[1] == ',':
            self.take()
        self.take(')')
        return chosen if found else None

    def pattern(self):
        if self.peek()[1] == '(':
            self.take('(')
            items = []
            while self.peek()[1] != ')':
                items.append(self.single_pattern())
                if self.peek()[1] == ',':
                    self.take()
            self.take(')')
            return tuple(items)
        return (self.single_pattern(),)

    def single_pattern(self):
        kind, val = self.take()
        if val == 'any' and self.peek()[1] == '@':
            self.take('@')
            self.take()
            return 'any'
        if kind == 'str':
            return ('str', val)
        if val in ('true', 'false'):
            return ('bool', val == 'true')
        return val  # default, any

    @staticmethod
    def matches(pattern, values):
        if pattern == ('default',):
            return True
        if len(pattern) != len(values):
            return False
        for p, v in zip(pattern, values):
            if p in ('default', 'any'):
                if p == 'any' and v is None:
                    return False
                continue
            if isinstance(v, bool) and p[0] == 'str':
                v = 'true' if v else 'false'
            if v is None or p[1] != v:
                return False
        return True

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
        if val == 'select' and self.peek(1)[1] == '(':
            return self.select()
        self.take()
        if kind in ('str', 'num'):
            return val
        if val in ('true', 'false'):
            return val == 'true'
        if val == 'unset':
            return Unset
        if kind == 'ident':
            return self.vars.get(val)
        raise BlueprintError(f'{self.path}: unexpected value {val!r}')

    def expr(self):
        v = self.value()
        while self.peek()[1] == '+':
            self.take()
            v = add(v, self.value())
        return v


def add(a, b):
    if a is None or a is Unset:
        return b
    if b is None or b is Unset:
        return a
    if isinstance(a, dict) and isinstance(b, dict):
        out = dict(a)
        for k, v in b.items():
            out[k] = add(out.get(k), v) if k in out else v
        return out
    return a + b


def parse_file(path, config):
    with open(path, encoding='utf-8') as f:
        text = f.read()
    return Parser(tokenize(text, path), path, config).file()


def apply_soong_config(mtype, props, config, module_types):
    """Folds a soong_config_module_type module's chosen variables into its
    properties."""
    spec = module_types.get(mtype)
    if not spec:
        return mtype, props
    namespace = spec.get('config_namespace', '')
    variables = props.pop('soong_config_variables', {}) or {}
    for var, branch in variables.items():
        if not isinstance(branch, dict):
            continue
        value = config.soong_config(namespace, var)
        default = branch.pop('conditions_default', {}) or {}
        if value in (None, False, ''):
            chosen = default
        elif var in (spec.get('value_variables') or []):
            chosen = {k: substitute(v, value) for k, v in branch.items()}
        elif var in (spec.get('variables') or []) and isinstance(value, str):
            chosen = branch.get(value, default)
        else:
            chosen = {k: v for k, v in branch.items() if not isinstance(v, dict)}
        for k, v in chosen.items():
            props[k] = v
    return spec.get('module_type', mtype), props


def substitute(v, value):
    if isinstance(v, str):
        return v.replace('%s', str(value))
    if isinstance(v, list):
        return [substitute(x, value) for x in v]
    return v
