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
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'gen'))

from blueprint import Config  # noqa: E402
from checkout import checkout  # noqa: E402
from plan import Planner  # noqa: E402
from tree import Modules, Tree, as_list, norm  # noqa: E402


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
        if not task.args.get('java', True):
            return  # only imported: its preprocessed form is made on demand
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

    def tool(self, name):
        """A host tool by its Soong module name (tools/host builds them)."""
        if name in ('aprotoc', 'protoc'):
            return os.path.join(self.args.host_tools, 'protobuf', 'protoc')
        if name == 'soong_zip':
            return os.path.join(HERE, 'soong_zip.py')
        return os.path.join(self.args.host_tools, name)

    @staticmethod
    def jarjar_rules(path):
        """(pattern, replacement) pairs of a jarjar rules file's `rule`s:
        `a.b.**` packages and single classes."""
        rules = []
        for line in open(path):
            parts = line.split()
            if len(parts) == 3 and parts[0] == 'rule':
                rules.append((parts[1], parts[2]))
        return rules

    @staticmethod
    def rename(text, rules):
        for pattern, replacement in rules:
            if pattern.endswith('.**') and replacement.endswith('.@1'):
                old, new = pattern[:-3], replacement[:-3]
                text = re.sub(r'(?<![\w.])' + re.escape(old) + r'(?=\.)', new, text)
            elif pattern.endswith('*') and replacement.endswith('@1'):
                old, new = pattern[:-1], replacement[:-2]
                text = re.sub(r'(?<![\w.])' + re.escape(old) + r'(?=\w)', new, text)
            elif '*' not in pattern:
                text = re.sub(r'(?<![\w.])' + re.escape(pattern) + r'(?![\w])', replacement, text)
        return text

    def jarjar_sources(self, plan):
        """A static library with jarjar rules is renamed before its
        dependents compile against it, as Soong jars it first: its sources
        and its own static libraries', and the code generated for them, are
        compiled renamed (copies; a source other libraries use as it is
        stays too)."""
        def closure(lib, seen):
            if lib in seen or lib not in plan.lib_files:
                return
            seen.add(lib)
            for dep in plan.lib_static.get(lib, []):
                closure(dep, seen)

        renamed_libs = {}
        for lib, rules_file in plan.jarjar.items():
            libs = set()
            closure(lib, libs)
            renamed_libs[lib] = libs
        in_closures = set().union(*renamed_libs.values()) if renamed_libs else set()
        plain_users = set()
        for lib, files in plan.lib_files.items():
            if lib not in in_closures:
                plain_users |= files
        self.renamed_out = set()
        for lib, libs in sorted(renamed_libs.items()):
            rules = self.jarjar_rules(self.path(plan.jarjar[lib]))
            out = os.path.join(self.gen, 'jarjar', safe(lib))
            shutil.rmtree(out, ignore_errors=True)
            for name in sorted(libs):
                for f in sorted(plan.lib_files[name]):
                    with open(self.path(f), encoding='utf-8') as fh:
                        text = self.rename(fh.read(), rules)
                    dst = os.path.join(out, f)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    with open(dst, 'w', encoding='utf-8') as fh:
                        fh.write(text)
                    if f not in plain_users:
                        self.renamed_out.add(f)
                for key in plan.lib_tasks.get(name, ()):
                    d = os.path.join(self.gen, safe(key))
                    for dp, _, fs in os.walk(d):
                        for fn in fs:
                            if fn.endswith('.java'):
                                path = os.path.join(dp, fn)
                                with open(path, encoding='utf-8') as fh:
                                    text = fh.read()
                                new = self.rename(text, rules)
                                if new != text:
                                    with open(path, 'w', encoding='utf-8') as fh:
                                        fh.write(new)
            self.java_dirs[True].append(out)

    def gen_proto(self, task):
        d = self.task_dir(task)
        kind = task.args['type']
        out_flag = {'nano': '--javanano_out', 'stream': '--javastream_out',
                    'micro': '--javamicro_out'}.get(kind, '--java_out')
        params = (['lite'] if kind in ('lite', None, '') else []) + task.args.get('output_params', [])
        flags = []
        if kind in ('nano', 'stream', 'micro'):
            flags.append(f'--plugin=protoc-gen-java{kind}={self.tool("protoc-gen-java" + kind)}')
        flags += [f'-I{i}' for i in task.args.get('includes', [])]
        for f in sorted(task.args['files']):
            base = '.' if task.args.get('canonical', True) else task.args['moddir']
            run([self.tool('aprotoc'), f'{out_flag}={",".join(params)}:{d}', '-I', base]
                + flags + [f], cwd=self.aosp)

    # Host tools' data files, where Soong installs them (out/host/.../etc),
    # as the tools find them from a genrule's sandbox.
    TOOL_DATA = {
        'stats-log-api-gen/java': 'frameworks/proto_logging/stats/stats_log_api_gen/include_java',
        'stats-log-api-gen/cc_hdrs': 'frameworks/proto_logging/stats/stats_log_api_gen/include_cc_hdrs',
        'stats-log-api-gen/cc_srcs': 'frameworks/proto_logging/stats/stats_log_api_gen/include_cc_srcs',
    }

    def sandbox(self):
        """A genrule's working directory, laid out like Soong's sandbox: the
        source tree, and the host tools' data under tools/out/etc."""
        box = os.path.join(self.out, 'sandbox')
        if not os.path.isdir(box):
            os.makedirs(box)
            for name in os.listdir(self.aosp):
                if name != 'tools':
                    os.symlink(os.path.join(self.aosp, name), os.path.join(box, name))
            tools = os.path.join(box, 'tools')
            os.makedirs(tools)
            if os.path.isdir(self.path('tools')):
                for name in os.listdir(self.path('tools')):
                    os.symlink(self.path(f'tools/{name}'), os.path.join(tools, name))
            for rel, src in self.TOOL_DATA.items():
                dst = os.path.join(tools, 'out', 'etc', rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.symlink(self.path(src), dst)
        return box

    def gen_genrule(self, task):
        a = task.args
        outs = as_list(a.get('out'))
        if outs and not any(o.endswith(('.java', '.srcjar')) for o in outs):
            return  # e.g. a mock initializer turned off by the release
        d = self.task_dir(task)
        gen_dir = os.path.join(self.out, 'genrule', safe(task.key))
        shutil.rmtree(gen_dir, ignore_errors=True)
        os.makedirs(gen_dir)

        def expand(cmd, ins, outs_):
            def location(m):
                name = m.group(2)
                if name in a.get('tools', []):
                    return self.tool(name)
                for f in a.get('tool_files', []):
                    if f == name or f.endswith('/' + name):
                        return f
                return self.tool(name)
            cmd = re.sub(r'\$\((locations?) ([^)]+)\)', location, cmd)
            cmd = cmd.replace('$(in)', ' '.join(ins)).replace('$(out)', ' '.join(outs_))
            cmd = cmd.replace('$(genDir)', gen_dir)
            return cmd

        if a.get('type') == 'gensrcs':
            ext = a.get('output_extension') or 'java'
            jobs = [([src], [os.path.join(gen_dir, os.path.splitext(src)[0] + '.' + ext)])
                    for src in a['srcs']]
        else:
            jobs = [(a['srcs'], [os.path.join(gen_dir, o) for o in outs])]
        env = {**os.environ, 'PATH': os.path.dirname(sys.executable) + os.pathsep + os.environ['PATH']}
        sandbox = self.sandbox()
        for ins, outs_ in jobs:
            for o in outs_:
                os.makedirs(os.path.dirname(o), exist_ok=True)
            cmd = expand(a['cmd'], ins, outs_)
            cmd = cmd.replace(self.tool('soong_zip'), f'{sys.executable} {self.tool("soong_zip")}')
            run(['bash', '-c', cmd], cwd=sandbox, env=env)
            for o in outs_:
                if o.endswith('.srcjar'):
                    with zipfile.ZipFile(o) as z:
                        z.extractall(d)
                elif o.endswith('.java'):
                    shutil.copy(o, os.path.join(d, os.path.basename(o)))

    def gen_sysprop(self, task):
        # The framework's sysprop libraries are the platform's own, on the
        # system partition: Soong generates their "internal" scope.
        d = self.task_dir(task)
        for f in task.args['files']:
            run([self.tool('sysprop_java'), '--scope', 'internal', '--java-output-dir', d,
                 self.path(f)])

    XSDC_COMMONS_CLI = 'https://repo1.maven.org/maven2/commons-cli/commons-cli/1.2/commons-cli-1.2.jar'

    def xsdc(self):
        """system/tools/xsdc, a Java tool, compiled on first use."""
        jar_dir = os.path.join(self.out, 'xsdc')
        classes = os.path.join(jar_dir, 'classes')
        cli = os.path.join(jar_dir, 'commons-cli-1.2.jar')
        if not os.path.isdir(classes):
            os.makedirs(jar_dir, exist_ok=True)
            if not os.path.isfile(cli):
                run(['curl', '-fsSL', '-o', cli, self.XSDC_COMMONS_CLI])
            srcs = []
            for dp, _, fs in os.walk(self.path('system/tools/xsdc/src/main/java')):
                srcs += [os.path.join(dp, f) for f in fs if f.endswith('.java')]
            tmp = classes + '.tmp'
            shutil.rmtree(tmp, ignore_errors=True)
            run([os.path.join(self.args.java_home, 'bin', 'javac'), '-nowarn', '-encoding', 'UTF-8',
                 '-cp', cli, '-d', tmp] + srcs)
            os.rename(tmp, classes)
        return [os.path.join(self.args.java_home, 'bin', 'java'), '-cp',
                os.pathsep.join([classes, cli]), 'com.android.xsdc.Main']

    def gen_xsd(self, task):
        d = self.task_dir(task)
        props = task.args['props']
        args = []
        for prop, flag in (('gen_writer', '-w'), ('nullability', '-n'), ('gen_has', '-g'),
                           ('enums_only', '-e'), ('parser_only', '-x'),
                           ('boolean_getter', '-b'), ('tinyxml', '-t')):
            if props.get(prop):
                args.append(flag)
        for elem in as_list(props.get('root_elements')):
            args += ['-r', elem]
        run(self.xsdc() + [self.path(task.args['src']), '-p', task.args['package'],
                           '-o', d, '-j'] + args)

    KOTLINC = 'https://github.com/JetBrains/kotlin/releases/download/v2.1.21/kotlin-compiler-2.1.21.zip'
    JAVAPOET = 'https://repo1.maven.org/maven2/com/squareup/javapoet/1.13.0/javapoet-1.13.0.jar'
    GUAVA = 'https://repo1.maven.org/maven2/com/google/guava/guava/33.3.1-jre/guava-33.3.1-jre.jar'

    def jvm_tools(self):
        """kotlinc (JetBrains' release), JavaPoet and Guava: what AOSP's
        Kotlin and Java build tools are compiled with. Fetched once."""
        d = os.path.join(self.out, 'jvm-tools')
        os.makedirs(d, exist_ok=True)
        kotlinc = os.path.join(d, 'kotlinc')
        if not os.path.isdir(kotlinc):
            z = os.path.join(d, 'kotlinc.zip')
            run(['curl', '-fsSL', '-o', z, self.KOTLINC])
            with zipfile.ZipFile(z) as f:
                f.extractall(d)
            os.chmod(os.path.join(kotlinc, 'bin', 'kotlinc'), 0o755)
        jars = {}
        for name, url in (('javapoet', self.JAVAPOET), ('guava', self.GUAVA)):
            jars[name] = os.path.join(d, f'{name}.jar')
            if not os.path.isfile(jars[name]):
                run(['curl', '-fsSL', '-o', jars[name], url])
        jars['kotlin-stdlib'] = os.path.join(kotlinc, 'lib', 'kotlin-stdlib.jar')
        return os.path.join(kotlinc, 'bin', 'kotlinc'), jars

    def compile_jvm(self, out, java=(), kotlin=(), classpath=()):
        """Compiles a host tool's Java, then its Kotlin, into `out`."""
        if os.path.isdir(out):
            return out
        tmp = out + '.tmp'
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        kotlinc, _ = self.jvm_tools()
        cp = os.pathsep.join(list(classpath) + [tmp])
        if java:
            run([os.path.join(self.args.java_home, 'bin', 'javac'), '-nowarn', '-proc:none',
                 '-encoding', 'UTF-8', '-cp', cp, '-d', tmp] + list(java))
        if kotlin:
            run([kotlinc] + list(kotlin) + ['-cp', cp, '-d', tmp, '-jvm-target', '17'],
                env={**os.environ, 'JAVA_HOME': self.args.java_home})
        os.rename(tmp, out)
        return out

    def systemfeatures_lookup(self, out_dir):
        """SystemFeaturesLookup.java, as systemfeatures-lookup-srcs makes it
        from the non-updatable API files: PackageManager's FEATURE_* string
        constants by value."""
        sys.path.insert(0, HERE)
        import api_stubs
        classes = {}
        for n in ('current.txt', 'module-lib-current.txt', 'system-current.txt', 'test-current.txt'):
            path = self.path(f'frameworks/base/core/api/{n}')
            if os.path.isfile(path):
                api_stubs.parse(path, classes)
        pm = classes.get('android.content.pm.PackageManager')
        cases = []
        for m in pm.members if pm else []:
            mm = re.match(r'^(String|java\.lang\.String) (FEATURE_\w+) = (".*")$', m.text)
            if m.kind == 'field' and mm and 'static' in m.mods:
                cases.append(f'            case {mm.group(3)}: return "{mm.group(2)}";')
        path = os.path.join(out_dir, 'com', 'android', 'systemfeatures', 'SystemFeaturesLookup.java')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', newline='\n') as f:
            f.write('// This file is auto-generated. DO NOT MODIFY.\n'
                    'package com.android.systemfeatures;\n\n'
                    '/** @hide */\npublic final class SystemFeaturesLookup {\n'
                    '    public static String getDeclaredFeatureVarNameFromValue(String featureVarValue) {\n'
                    '        switch (featureVarValue) {\n' + '\n'.join(dict.fromkeys(cases)) +
                    '\n            default: return null;\n        }\n    }\n}\n')
        return path

    def systemfeatures_tool(self):
        """frameworks/base/tools/systemfeatures' generator (Kotlin)."""
        _, jars = self.jvm_tools()
        d = os.path.join(self.out, 'systemfeatures-tool')
        lookup_src = os.path.join(d, 'lookup-src')
        lookup = self.compile_jvm(os.path.join(d, 'lookup'),
                                  java=[self.systemfeatures_lookup(lookup_src)])
        src = self.path('frameworks/base/tools/systemfeatures/src/com/android/systemfeatures/'
                        'SystemFeaturesGenerator.kt')
        cp = [lookup, jars['javapoet'], jars['guava'], jars['kotlin-stdlib']]
        gen = self.compile_jvm(os.path.join(d, 'classes'), kotlin=[src], classpath=cp)
        return [os.path.join(self.args.java_home, 'bin', 'java'), '-cp',
                os.pathsep.join([gen] + cp), 'com.android.systemfeatures.SystemFeaturesGenerator']

    def gen_systemfeatures(self, task):
        # As java_system_features_srcs does: features from RELEASE_SYSTEM_FEATURE_*
        # build flags (none in Gesher's builds), read-only per
        # RELEASE_USE_SYSTEM_FEATURE_BUILD_FLAGS, and no device feature files.
        d = self.task_dir(task)
        flags = self.config.flags
        # A flag declared without a value is "" in Soong's BuildFlags: the
        # feature's status is left to run time.
        features = sorted(f'{k[len("RELEASE_SYSTEM_FEATURE_"):]}:{"" if v is None else v}'
                          for k, v in flags.items() if k.startswith('RELEASE_SYSTEM_FEATURE_'))
        readonly = 'true' if flags.get('RELEASE_USE_SYSTEM_FEATURE_BUILD_FLAGS') else 'false'
        cls = task.args['cls']
        r = run(self.systemfeatures_tool() + [cls] + [f'--feature={f}' for f in features]
                + [f'--readonly={readonly}', '--metadata-only=false'])
        out = os.path.join(d, *cls.split('.')) + '.java'
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'w') as f:
            f.write(r.stdout)

    def hidl_roots(self):
        """-r<package root>:<path> for every hidl_package_root, as hidl-gen
        resolves imported packages through them."""
        roots = []
        for mod in self.modules.by_name.values():
            if mod.type == 'hidl_package_root':
                path = mod.props.get('path')
                path = norm(f'{mod.dir}/{path}') if path else mod.dir
                roots.append(f'-r{mod.name}:{self.path(path)}')
        return sorted(roots)

    def gen_hidl(self, task):
        d = self.task_dir(task)
        lang = 'java-constants' if task.args.get('constants') else 'java'
        run([self.tool('hidl-gen'), '-o', d, '-L', lang] + self.hidl_roots()
            + [task.args['fqname']], cwd=self.aosp)

    # Annotation processors that generate code the framework compiles
    # against (java_plugin modules); the others in `plugins` only check it.
    PROCESSORS = {
        'cached-property-annotation-processor': {
            'class': 'android.processor.property_cache.CachedPropertyProcessor',
            'srcs': ['frameworks/base/tools/processors/property_cache/src/java'],
            'files': ['frameworks/libs/modules-utils/java/com/android/internal/annotations/CacheModifier.java',
                      'frameworks/libs/modules-utils/java/com/android/internal/annotations/CachedProperty.java',
                      'frameworks/libs/modules-utils/java/com/android/internal/annotations/'
                      'CachedPropertyDefaults.java'],
            'jars': ['guava'],
        },
        'systemfeatures-metadata-processor': {
            'class': 'com.android.systemfeatures.SystemFeaturesMetadataProcessor',
            'srcs': [],
            # framework-metalava-annotations' SdkConstant, which it reads.
            'files': ['frameworks/libs/modules-utils/java/android/annotation/SdkConstant.java'],
            'kotlin': ['frameworks/base/tools/systemfeatures/src/com/android/systemfeatures/'
                       'SystemFeaturesMetadataProcessor.kt'],
            'jars': ['javapoet', 'guava', 'kotlin-stdlib'],
        },
    }

    def processors(self, plan):
        """-processorpath and -processor for the plugins that generate code."""
        used = [p for p in sorted(plan.plugins) if p in self.PROCESSORS]
        if not used:
            return ['-proc:none']
        _, jars = self.jvm_tools()
        paths, classes = [], []
        for name in used:
            spec = self.PROCESSORS[name]
            java = [self.path(f) for f in spec['files']]
            for d in spec['srcs']:
                java += [os.path.join(dp, f) for dp, _, fs in os.walk(self.path(d))
                         for f in fs if f.endswith('.java')]
            deps = [jars[j] for j in spec.get('jars', [])]
            out = self.compile_jvm(os.path.join(self.out, 'processors', name), java=java,
                                   kotlin=[self.path(k) for k in spec.get('kotlin', [])],
                                   classpath=deps)
            paths += [out] + [d for d in deps if d not in paths]
            classes.append(spec['class'])
        return ['-processorpath', os.pathsep.join(paths), '-processor', ','.join(classes),
                '-s', os.path.join(self.out, 'apt')]

    # --- the build ------------------------------------------------------------
    def build(self):
        tree = Tree(self.aosp)
        self.config = Config(self.path('build/release'), self.args.release)
        modules = self.modules = Modules(tree, self.config)
        planner = Planner(tree, modules)
        planner.core_platform()
        planner.library(self.args.module)
        plan = self.plan = planner.plan
        if self.args.checkout:
            checkout(tree, plan.needed | {
                'frameworks/base/core/api/current.txt', 'libcore/api/current.txt',
                'build/make/tools/java-event-log-tags.py', 'build/make/tools/event_log_tags.py'}
                     | {f for spec in self.PROCESSORS.values()
                        for f in spec['files'] + spec.get('kotlin', [])},
                     plan.include_dirs | {d for spec in self.PROCESSORS.values() for d in spec['srcs']})
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
        self.jarjar_sources(plan)
        print('generated:', dict(counts))
        for why, keys in sorted(self.skipped.items()):
            print(f'  skipped ({why}): {len(keys)}')
            for k in keys[:self.args.show]:
                print(f'    {k}')
        self.javac(plan)

    @staticmethod
    def class_name(path):
        with open(path, encoding='utf-8', errors='replace') as f:
            m = re.search(r'^\s*package\s+([\w.]+)\s*;', f.read(), re.M)
        return (m.group(1) if m else ''), os.path.splitext(os.path.basename(path))[0]

    def javac(self, plan):
        bundled = [self.path(f) for f in sorted(plan.bundle - getattr(self, 'renamed_out', set()))]
        stubs, others = [], [self.path(f) for f in sorted(plan.compile - plan.bundle)]
        for bundle, dirs in self.java_dirs.items():
            for d in dirs:
                files = [os.path.join(dp, f) for dp, _, fs in os.walk(d) for f in sorted(fs)
                         if f.endswith('.java')]
                if bundle:
                    bundled += files
                elif os.path.basename(d) == 'api-stubs':
                    stubs += files
                else:
                    others += files
        # One source per class, as one jar holds one class: what the jar
        # bundles, then the API stubs of libraries it compiles against, then
        # other compile-only sources. Packages libcore's java.base already
        # has (its aconfig flag classes) come from there.
        core = self.args.core_classes
        seen, sources = set(), {True: [], False: []}
        dropped = collections.Counter()
        core_packages = {os.path.relpath(dp, core).replace(os.sep, '.')
                         for dp, _, fs in os.walk(core) if any(f.endswith('.class') for f in fs)}
        for bundle, files in ((True, bundled), (False, stubs), (False, others)):
            for f in files:
                pkg, cls = self.class_name(f)
                if pkg in core_packages:
                    dropped['in java.base'] += 1
                    continue
                if (pkg, cls) in seen:
                    dropped['duplicate'] += 1
                    continue
                seen.add((pkg, cls))
                sources[bundle].append(f)
        print(f'sources left out: {dict(dropped)}')
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
               '-encoding', 'UTF-8', '-nowarn', '-Xmaxerrs', str(self.args.max_errors),
               '-XDstringConcat=inline',
               '-cp', os.pathsep.join(self.args.classpath + [self.path(j) for j in sorted(plan.jars)]),
               '-d', classes]
        cmd += self.processors(plan)
        os.makedirs(os.path.join(self.out, 'apt'), exist_ok=True)
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
    ap.add_argument('--host-tools', default='out/host', help='tools/host build directory')
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
