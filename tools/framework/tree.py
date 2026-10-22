"""The AOSP source tree: projects, their files, globs, and Soong modules.

Paths are AOSP-relative (`frameworks/base/core/java/...`). File lists come
from each project's git tree (`git ls-tree`), so globs resolve before the
files are checked out; .bp files must be checked out.
"""
import fnmatch
import os
import re
import subprocess
import sys

from blueprint import BlueprintError, apply_soong_config, parse_file


class Tree:
    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.projects = []
        for dirpath, dirnames, _ in os.walk(self.root):
            if '.git' in dirnames:
                self.projects.append(os.path.relpath(dirpath, self.root).replace(os.sep, '/'))
                dirnames[:] = [d for d in dirnames if d != '.git']
                # Projects nest (e.g. frameworks/libs/...): keep walking.
            dirnames[:] = [d for d in dirnames if not d.startswith('.')]
        self.projects.sort(key=lambda p: -len(p))
        self._files = {}

    def project_of(self, path):
        for p in self.projects:
            if path == p or path.startswith(p + '/'):
                return p
        return None

    def files(self, project):
        if project not in self._files:
            out = subprocess.run(
                ['git', '-C', os.path.join(self.root, project), 'ls-tree', '-r',
                 '--name-only', '-z', 'HEAD'],
                check=True, capture_output=True).stdout.decode('utf-8', 'replace')
            self._files[project] = [f'{project}/{f}' for f in out.split('\0') if f]
        return self._files[project]

    def exists(self, path):
        project = self.project_of(path)
        return project is not None and path in self.file_set(project)

    def file_set(self, project):
        key = ('set', project)
        if key not in self._files:
            self._files[key] = set(self.files(project))
        return self._files[key]

    def glob(self, base, pattern):
        """Files under `base` matching a Soong glob (`*`, `**`, `?`, `[...]`)."""
        full = norm(f'{base}/{pattern}')
        project = self.project_of(full)
        if project is None:
            return []
        regex = re.compile(glob_regex(full))
        return sorted(f for f in self.files(project) if regex.fullmatch(f))

    def dir_files(self, directory):
        directory = norm(directory)
        project = self.project_of(directory)
        if project is None:
            return []
        prefix = directory + '/'
        return [f for f in self.files(project) if f.startswith(prefix)]


def norm(path):
    parts = []
    for part in path.replace('\\', '/').split('/'):
        if part in ('', '.'):
            continue
        if part == '..':
            if parts:
                parts.pop()
            continue
        parts.append(part)
    return '/'.join(parts)


def glob_regex(pattern):
    out, i = [], 0
    while i < len(pattern):
        c = pattern[i]
        if pattern.startswith('**/', i):
            out.append('(?:.*/)?')
            i += 3
            continue
        if pattern.startswith('**', i):
            out.append('.*')
            i += 2
            continue
        if c == '*':
            out.append('[^/]*')
        elif c == '?':
            out.append('[^/]')
        elif c == '[':
            j = pattern.index(']', i)
            out.append(fnmatch.translate(pattern[i:j + 1])[4:-3])
            i = j
        else:
            out.append(re.escape(c))
        i += 1
    return ''.join(out)


class Module:
    def __init__(self, mtype, props, directory):
        self.type, self.props, self.dir = mtype, props, directory
        self.name = props.get('name')

    def __repr__(self):
        return f'{self.type}({self.name} in {self.dir})'


PREBUILT_TYPES = ('prebuilt', '_import', 'sdk_library_import')


class Modules:
    def __init__(self, tree, config):
        self.tree, self.config = tree, config
        self.by_name = {}
        self.dupes = {}
        parsed = []
        module_types = {}
        for dirpath, dirnames, filenames in os.walk(tree.root):
            dirnames[:] = [d for d in dirnames if not d.startswith('.')]
            for f in sorted(filenames):
                if not f.endswith('.bp'):
                    continue
                path = os.path.join(dirpath, f)
                rel = norm(os.path.relpath(dirpath, tree.root))
                try:
                    mods = parse_file(path, config)
                except (BlueprintError, UnicodeDecodeError, IndexError) as e:
                    print(f'warning: {e}', file=sys.stderr)
                    continue
                for mtype, props in mods:
                    if mtype == 'soong_config_module_type':
                        module_types[props.get('name')] = props
                    parsed.append((mtype, props, rel))
        for mtype, props, rel in parsed:
            mtype, props = apply_soong_config(mtype, props, config, module_types)
            name = props.get('name')
            if not isinstance(name, str):
                continue
            mod = Module(mtype, props, rel)
            old = self.by_name.get(name)
            if old is None or (is_prebuilt(old) and not is_prebuilt(mod)):
                self.by_name[name] = mod
            if old is not None:
                self.dupes.setdefault(name, []).append(mod)

    def get(self, name):
        return self.by_name.get(name)

    def merged(self, mod):
        """The module's properties with its `defaults` applied, as Soong
        does: lists from defaults come first, the module's values win."""
        props = {}
        for d in as_list(mod.props.get('defaults')):
            dm = self.get(d)
            if dm is not None:
                props = merge(props, self.merged(dm))
        return merge(props, {k: v for k, v in mod.props.items() if k != 'defaults'})


def is_prebuilt(mod):
    return any(t in mod.type for t in PREBUILT_TYPES)


def merge(base, over):
    out = dict(base)
    for k, v in over.items():
        if k in out and isinstance(out[k], list) and isinstance(v, list):
            out[k] = out[k] + v
        elif k in out and isinstance(out[k], dict) and isinstance(v, dict):
            out[k] = merge(out[k], v)
        else:
            out[k] = v
    return out


def as_list(value):
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    return [v for v in value if isinstance(v, str)]
