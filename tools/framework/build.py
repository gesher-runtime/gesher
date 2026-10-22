#!/usr/bin/env python3
"""Builds a framework jar from AOSP sources, the way Soong would.

    build.py --aosp <dir> --out <dir> --core-classes <dir> [options] [MODULE]

Plans MODULE (default framework-minus-apex; tools/framework/plan.py), checks
out the files it needs (--checkout), runs each generator into <out>/gen,
compiles the bundled and compile-only sources in one javac run against
libcore's classes (the java.base module tools/build-boot-jars.sh compiles,
kept with its WORK_DIR), and writes <out>/<module>.classes.txt, the classes
that go into the jar. Generators Gesher lacks are reported, and javac's
errors summarized, so the build can be grown step by step.
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'gen'))

from blueprint import Config  # noqa: E402
from checkout import checkout  # noqa: E402
from plan import Planner  # noqa: E402
from tree import Modules, Tree  # noqa: E402


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        out = r.stdout + r.stderr
        raise RuntimeError(f'{cmd[0]} failed ({r.returncode}):\n{out[:3000]}\n...\n{out[-1000:]}')
    return r


def safe(key):
    return re.sub(r'[^\w.@-]+', '_', key)


class Builder:
    def __init__(self, args):
        self.args = args
        self.aosp = os.path.abspath(args.aosp)
        self.out = os.path.abspath(args.out)
        self.gen = os.path.join(self.out, 'gen')
        self.skipped = collections.defaultdict(list)
        self.java_dirs = {True: [], False: []}  # bundle -> generated source dirs
        self.preprocessed_done = set()

    def path(self, rel):
        return os.path.join(self.aosp, rel)

    def task_dir(self, task):
        d = os.path.join(self.gen, safe(task.key))
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        self.java_dirs[task.bundle].append(d)
        return d

    # --- generators -------------------------------------------------------
    def gen_aconfig(self, task):
        d = self.task_dir(task)
        values = os.path.join(self.out, 'aconfig.values')
        r = run([sys.executable, os.path.join(HERE, '..', 'gen', 'aconfig_values.py'),
                 self.path('build/release'), self.args.release])
        with open(values, 'w') as f:
            f.write(r.stdout)
        run([sys.executable, os.path.join(HERE, '..', 'gen', 'aconfig_java.py'), values, d]
            + [self.path(p) for p in sorted(task.args['files'])])

    def aidl_cmd(self, task, includes):
        # A platform build: the framework's SDK level, as Soong's "current".
        cmd = [self.args.aidl, '--lang=java', f'--min_sdk_version={self.args.sdk}']
        cmd += task.args.get('flags') or []
        if task.args.get('structured'):
            cmd.append('--structured')
        if task.args.get('stability'):
            cmd.append(f'--stability={task.args["stability"]}')
        version = task.args.get('version')
        if version and task.kind == 'aidl-interface':
            cmd.append(f'--version={version}')
            hash_file = task.args.get('hash_file')
            if hash_file and os.path.isfile(self.path(hash_file)):
                with open(self.path(hash_file)) as f:
                    lines = [x.strip() for x in f if x.strip()]
                if lines:
                    cmd.append(f'--hash={lines[-1]}')
            else:
                cmd.append('--hash=notfrozen')
        dirs = {self.path(i) for i in includes}
        if task.kind == 'aidl':
            # Soong's java_library adds the roots of its .aidl sources.
            dirs.update(self.package_root(self.path(f)) for f in task.args['files'])
        cmd += [f'-I{d}' for d in sorted(d for d in dirs if d and os.path.isdir(d))]
        return cmd

    @staticmethod
    def package_root(path):
        """The directory a .aidl file's package path starts in."""
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                m = re.search(r'^\s*package\s+([\w.]+)\s*;', f.read(), re.M)
        except OSError:
            return None
        root = os.path.dirname(path)
        for part in reversed(m.group(1).split('.') if m else []):
            if os.path.basename(root) != part:
                return None
            root = os.path.dirname(root)
        return root

    def gen_aidl(self, task, includes):
        d = self.task_dir(task)
        files = [self.path(f) for f in task.args['files']]
        if not files:
            return
        run(self.aidl_cmd(task, includes) + ['-o', d] + files)

    def interface_flags(self, task):
        flags = []
        if task.args.get('root'):
            flags.append(f'-N{self.path(task.args["root"])}')
        for key in task.args.get('imports') or []:
            flags += ['-p', self.preprocessed(key)]
        return flags

    def preprocessed(self, key):
        """An interface version's types as one preprocessed .aidl file, the
        form its importers read it in."""
        out = os.path.join(self.out, 'preprocessed', safe(key) + '.aidl')
        if key not in self.preprocessed_done:
            task = self.plan.tasks[key]
            os.makedirs(os.path.dirname(out), exist_ok=True)
            cmd = [self.args.aidl, '--preprocess', out]
            if task.args.get('structured'):
                cmd.append('--structured')
            if task.args.get('stability'):
                cmd.append(f'--stability={task.args["stability"]}')
            cmd += [f'-I{self.path(i)}' for i in sorted(self.plan.aidl_includes.get(key, ()))]
            cmd += self.interface_flags(task)
            run(cmd + [self.path(f) for f in task.args['files']])
            self.preprocessed_done.add(key)
        return out

    def gen_aidl_interface(self, task, includes):
        d = self.task_dir(task)
        files = [self.path(f) for f in task.args['files']]
        if files:
            run(self.aidl_cmd(task, includes) + self.interface_flags(task) + ['-o', d] + files)

    def gen_framework_res(self, task):
        src = self.args.framework_res_java
        if not src:
            self.skipped['framework-res: pass --framework-res-java'].append(task.key)
            return
        d = self.task_dir(task)
        shutil.copytree(src, d, dirs_exist_ok=True)

    def gen_logtags(self, task):
        d = self.task_dir(task)
        tool = self.path('build/make/tools/java-event-log-tags.py')
        for i, f in enumerate(sorted(task.args['files'])):
            base = os.path.splitext(os.path.basename(f))[0]
            out = os.path.join(d, f'{i}', f'{base}.java')
            os.makedirs(os.path.dirname(out))
            run([sys.executable, tool, '-o', out, self.path(f)],
                env={**os.environ, 'PYTHONPATH': os.path.dirname(tool)})

    def gen_api_stubs(self, task):
        d = self.task_dir(task)
        known = [self.path(f'frameworks/base/core/api/{n}') for n in
                 ('current.txt', 'system-current.txt', 'module-lib-current.txt')]
        known += [self.path(f'libcore/api/{n}') for n in ('current.txt', 'module-lib-current.txt')]
        known = [k for k in known if os.path.isfile(k)]
        run([sys.executable, os.path.join(HERE, 'api_stubs.py'), d]
            + [self.path(f) for f in task.args['files']] + ['--known'] + known)

    # --- the build ------------------------------------------------------------
    def build(self):
        tree = Tree(self.aosp)
        modules = Modules(tree, Config(self.path('build/release'), self.args.release))
        planner = Planner(tree, modules)
        planner.library(self.args.module)
        plan = self.plan = planner.plan
        if self.args.checkout:
            checkout(tree, plan.needed | {
                'frameworks/base/core/api/current.txt', 'libcore/api/current.txt',
                'build/make/tools/java-event-log-tags.py', 'build/make/tools/event_log_tags.py'},
                     plan.include_dirs)
        print(f'plan: {len(plan.bundle)} bundled + {len(plan.compile)} compile-only sources, '
              f'{len(plan.tasks)} generation tasks; missing {len(plan.missing)}, '
              f'unsupported {len(plan.unsupported)}')
        for what, refs in sorted(plan.unsupported.items()):
            print(f'  unsupported: {what}')
        os.makedirs(self.gen, exist_ok=True)
        counts = collections.Counter()
        for task in plan.tasks.values():
            gen = getattr(self, 'gen_' + task.kind.replace('-', '_'), None)
            if gen is None:
                self.skipped[f'no generator for {task.kind}'].append(task.key)
                continue
            try:
                if task.kind in ('aidl', 'aidl-interface'):
                    gen(task, plan.aidl_includes.get(task.key, ()))
                else:
                    gen(task)
                counts[task.kind] += 1
            except RuntimeError as e:
                first = next((ln for ln in str(e).splitlines() if 'ERROR' in ln or 'rror:' in ln),
                             str(e)[:400])
                self.skipped[f'{task.kind} failed'].append(f'{task.key}: {first[:600]}')
        print('generated:', dict(counts))
        for why, keys in sorted(self.skipped.items()):
            print(f'  skipped ({why}): {len(keys)}')
            for k in keys[:self.args.show]:
                print(f'    {k}')
        self.javac(plan)

    def javac(self, plan):
        sources = {True: sorted(self.path(f) for f in plan.bundle),
                   False: sorted(self.path(f) for f in plan.compile - plan.bundle)}
        for bundle, dirs in self.java_dirs.items():
            for d in dirs:
                for dirpath, _, files in os.walk(d):
                    sources[bundle] += [os.path.join(dirpath, f) for f in files if f.endswith('.java')]
        lists = {}
        for bundle, files in sources.items():
            name = 'bundle' if bundle else 'compile'
            lists[name] = os.path.join(self.out, f'{name}.sources.txt')
            with open(lists[name], 'w') as f:
                f.write('\n'.join(files) + '\n')
        argfile = os.path.join(self.out, 'javac.args')
        with open(argfile, 'w') as f:
            f.write('\n'.join(sources[True] + sources[False]) + '\n')
        classes = os.path.join(self.out, 'classes')
        shutil.rmtree(classes, ignore_errors=True)
        os.makedirs(classes)
        core = self.args.core_classes
        exports = sorted({os.path.relpath(dp, core).replace(os.sep, '.')
                          for dp, _, fs in os.walk(core) if any(x.endswith('.class') for x in fs)})
        cmd = [os.path.join(self.args.java_home, 'bin', 'javac'), f'-J-Xmx{self.args.javac_memory}',
               '-source', '21', '-target', '21', '--system=none', '--module-path', core,
               '-encoding', 'UTF-8', '-nowarn', '-proc:none', '-Xmaxerrs', str(self.args.max_errors),
               '-XDstringConcat=inline', '-cp', os.pathsep.join(self.args.classpath),
               '-d', classes]
        for pkg in exports:
            if pkg != '.':
                cmd += ['--add-exports', f'java.base/{pkg}=ALL-UNNAMED']
        print(f'javac: {len(sources[True])} bundled + {len(sources[False])} compile-only files')
        r = subprocess.run(cmd + [f'@{argfile}'], capture_output=True, text=True)
        log = os.path.join(self.out, 'javac.log')
        with open(log, 'w') as f:
            f.write(r.stdout + r.stderr)
        errors = re.findall(r'^(\S+?\.java):\d+: error: (.*)$', r.stdout + r.stderr, re.M)
        print(f'javac exit {r.returncode}, {len(errors)} errors (log: {log})')
        summary = collections.Counter(re.sub(r'\b[A-Z]\w*(\.\w+)*', 'X', e[1]) for e in errors)
        for msg, n in summary.most_common(15):
            print(f'  {n:6}  {msg}')
        if r.returncode == 0:
            run([sys.executable, os.path.join(HERE, '..', 'gen', 'split_classes.py'), classes,
                 os.path.join(self.out, 'split'), f'{self.args.module}={lists["bundle"]}'])
        return r.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('module', nargs='?', default='framework-minus-apex')
    ap.add_argument('--aosp', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--release', default='bp4a')
    ap.add_argument('--sdk', default='36')
    ap.add_argument('--core-classes', required=True)
    ap.add_argument('--classpath', action='append', default=[])
    ap.add_argument('--aidl', default='aidl')
    ap.add_argument('--framework-res-java')
    ap.add_argument('--java-home', default=os.environ.get('JAVA_HOME', '/usr'))
    ap.add_argument('--javac-memory', default='6g')
    ap.add_argument('--max-errors', type=int, default=100000)
    ap.add_argument('--checkout', action='store_true')
    ap.add_argument('--show', type=int, default=5)
    args = ap.parse_args()
    sys.exit(Builder(args).build())


if __name__ == '__main__':
    main()
