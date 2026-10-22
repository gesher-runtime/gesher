"""Plans a Java library's build from its Soong module graph.

Walks a module the way Soong builds a java_library: its sources, the
modules its `srcs` reference, `static_libs` (compiled into the library) and
`libs` (on the class path only), and the code Soong generates on the way
(AIDL, HIDL, aconfig, protos, sysprop, XSD, genrules). The result is a Plan:
Java sources to bundle, sources and jars needed only to compile, generation
tasks, and the files to check out. Module kinds Gesher cannot build yet are
reported, not guessed.
"""
import re
from collections import defaultdict

from tree import as_list, norm

JAVA_LIB_TYPES = {
    'java_library', 'java_library_static', 'java_library_host', 'android_library',
    'java_sdk_library', 'java_test_helper_library',
}


class Task:
    """Code generation for one module (or one interface version)."""

    def __init__(self, kind, key, **args):
        self.kind, self.key, self.args = kind, key, args
        self.bundle = False

    def __repr__(self):
        return f'Task({self.kind}, {self.key})'


class Plan:
    def __init__(self):
        self.bundle = set()        # Java sources compiled into the library
        self.compile = set()       # Java sources needed only to compile
        self.jars = set()          # prebuilt jars, compile only
        self.tasks = {}            # key -> Task
        self.needed = set()        # files to check out
        self.unsupported = defaultdict(set)  # what -> referrers
        self.missing = defaultdict(set)      # module name -> referrers
        self.aidl_includes = defaultdict(set)  # task key -> -I dirs
        self.include_dirs = set()  # directories generators search: checked out whole
        self.jarjar = {}           # library -> its jarjar rules file (sources renamed)
        self.jarjar_tasks = defaultdict(set)  # library -> generation tasks it renames

    def add_java(self, path, bundle):
        self.needed.add(path)
        (self.bundle if bundle else self.compile).add(path)

    def task(self, kind, key, bundle, **args):
        t = self.tasks.get(key)
        if t is None:
            t = self.tasks[key] = Task(kind, key, **args)
        t.bundle = t.bundle or bundle
        return t


# Soong's combined module APIs (frameworks/base/api/api.go), by the
# combined_apis module that lists their libraries.
COMBINED_STUBS = {
    'framework-updatable-stubs-module_libs_api': 'frameworks-base-api',
}
# Already on the class path: libcore's, ICU's and Conscrypt's boot jars.
CORE_APIS = {'art.module.public.api', 'conscrypt.module.public.api', 'i18n.module.public.api'}
SDK_LIBRARY_VARIANT = re.compile(r'^(.+?)(\.impl|\.stubs(?:\.[a-z_]+)*|\.module_lib)$')
API_FILES = ('current.txt', 'system-current.txt', 'module-lib-current.txt')
# Libraries whose jarjar rules rename what their dependents compile against:
# their generated sources are renamed before javac (tools/framework/build.py).
JARJAR_BEFORE_DEPENDENTS = {'perfetto_trace_javastream_protos_jarjar'}

AIDL_VERSIONED = re.compile(
    r'^(?P<name>.+?)(?:-V(?P<version>\d+))?(?P<suffix>-java-source|-java_static|-java|-java-shallow)$')
HIDL_JAVA = re.compile(r'^(?P<pkg>.+)-V(?P<ver>\d+\.\d+)-java(?P<constants>-constants)?$')


class Planner:
    def __init__(self, tree, modules):
        self.tree, self.modules = tree, modules
        self.plan = Plan()
        self.done = {}  # (name, bundle) -> True
        self.hidl_roots = {}
        for mod in modules.by_name.values():
            if mod.type == 'hidl_package_root':
                path = mod.props.get('path') or mod.dir
                self.hidl_roots[mod.name] = norm(path) if '/' in str(path) and not path.startswith(mod.dir) else norm(f'{mod.dir}/{path}') if mod.props.get('path') else mod.dir

    # --- entry points ---------------------------------------------------
    def library(self, name, bundle=True, referrer=None):
        key = (name, bundle)
        if key in self.done or (name, True) in self.done:
            return
        self.done[key] = True
        mod = self.modules.get(name)
        if mod is None:
            generated = self.generated_library(name, bundle, referrer)
            if not generated:
                self.plan.missing[name].add(referrer)
            return
        props = self.modules.merged(mod)
        if mod.type in JAVA_LIB_TYPES:
            self.java_library(mod, props, bundle)
        elif mod.type == 'java_import':
            for jar in as_list(props.get('jars')):
                path = norm(f'{mod.dir}/{jar}')
                self.plan.jars.add(path)
                self.plan.needed.add(path)
            for dep in as_list(props.get('static_libs')):
                self.library(dep, False, name)
        elif mod.type == 'java_aconfig_library':
            self.aconfig(props.get('aconfig_declarations'), bundle, name)
        elif mod.type == 'sysprop_library':
            self.sysprop(mod, props, bundle)
        elif mod.type == 'aidl_interface':
            self.aidl_interface(mod, props, None, bundle, name)
        elif mod.type in ('java_defaults',):
            pass
        else:
            self.plan.unsupported[f'library type {mod.type} ({name})'].add(referrer)

    def generated_library(self, name, bundle, referrer):
        """Libraries Soong derives from other modules."""
        if name in COMBINED_STUBS:
            api = self.modules.get(COMBINED_STUBS[name])
            for lib in as_list(api.props.get('bootclasspath')) if api else []:
                if lib not in CORE_APIS:
                    self.sdk_library_stubs(lib, referrer)
            return True
        m = SDK_LIBRARY_VARIANT.match(name)
        if m and self.modules.get(m.group(1)) is not None:
            if m.group(2) == '.impl' and bundle:
                self.library(m.group(1), bundle, referrer)
            else:
                self.sdk_library_stubs(m.group(1), referrer)
            return True
        m = AIDL_VERSIONED.match(name)
        if m and self.modules.get(m.group('name')) is not None:
            mod = self.modules.get(m.group('name'))
            if mod.type == 'aidl_interface':
                self.aidl_interface(mod, self.modules.merged(mod), m.group('version'),
                                    bundle, referrer)
                return True
        m = HIDL_JAVA.match(name)
        if m:
            fq = f'{m.group("pkg")}@{m.group("ver")}'
            mod = self.modules.get(fq)
            if mod is not None and mod.type == 'hidl_interface':
                self.hidl(mod, bool(m.group('constants')), bundle, referrer)
                return True
        return False

    def sdk_library_stubs(self, name, referrer):
        """A java_sdk_library's API, compile only: stubs from its signature
        files (tools/framework/api_stubs.py), as java_api_library does."""
        mod = self.modules.get(name)
        if mod is None:
            self.plan.missing[name].add(referrer)
            return
        props = self.modules.merged(mod)
        api_dir = norm(f'{mod.dir}/{props.get("api_dir", "api")}')
        files = [f'{api_dir}/{n}' for n in API_FILES if self.tree.exists(f'{api_dir}/{n}')]
        if not files:
            self.plan.unsupported[f'no API files for {name}'].add(referrer)
            return
        t = self.plan.task('api-stubs', 'api-stubs', False, files=[])
        if not set(files) <= set(t.args['files']):
            t.args['files'] += files
        self.plan.needed.update(files)

    # --- java_library -------------------------------------------------------
    def java_library(self, mod, props, bundle):
        aidl = props.get('aidl') or {}
        includes = [norm(f'{mod.dir}/{d}') for d in as_list(aidl.get('local_include_dirs'))]
        includes += [norm(d) for d in as_list(aidl.get('include_dirs'))]
        includes += [norm(f'{mod.dir}/{d}') for d in as_list(aidl.get('export_include_dirs'))]
        includes.append(mod.dir)
        if self.tree.dir_files(f'{mod.dir}/src'):
            includes.append(f'{mod.dir}/src')
        for dep in as_list(props.get('static_libs')) + as_list(props.get('libs')):
            includes += self.exported_aidl_includes(dep)
        aidl_flags = []
        if aidl.get('generate_get_transaction_name'):
            aidl_flags.append('--transaction_names')
        self.plan.include_dirs.update(includes)
        ctx = Context(mod, props, bundle, includes, aidl_flags)
        excluded = set(self.expand(ctx, as_list(props.get('exclude_srcs')), collect=False))
        for item in self.expand(ctx, as_list(props.get('srcs')) + as_list(props.get('device_common_srcs'))):
            if item not in excluded:
                self.source(ctx, item)
        self.plan.include_dirs.update(ctx.includes)
        rules = props.get('jarjar_rules')
        if rules and mod.name in JARJAR_BEFORE_DEPENDENTS:
            path = self.expand(ctx, [rules], collect=False)
            if path:
                self.plan.jarjar[mod.name] = path[0]
                self.plan.needed.add(path[0])
                self.plan.jarjar_tasks[mod.name].update(ctx.tasks)
        for dep in as_list(props.get('static_libs')):
            self.library(dep, bundle, mod.name)
        for dep in as_list(props.get('libs')):
            self.library(dep, False, mod.name)

    def exported_aidl_includes(self, name, seen=None):
        seen = seen or set()
        if name in seen:
            return []
        seen.add(name)
        mod = self.modules.get(name)
        if mod is None:
            return []
        props = self.modules.merged(mod)
        aidl = props.get('aidl') or {}
        dirs = [norm(f'{mod.dir}/{d}') for d in as_list(aidl.get('export_include_dirs'))]
        for dep in as_list(props.get('static_libs')) + as_list(props.get('libs')):
            dirs += self.exported_aidl_includes(dep, seen)
        return dirs

    def expand(self, ctx, entries, collect=True):
        """Source entries -> AOSP paths of files (or generated items)."""
        out = []
        for e in entries:
            if e.startswith(':'):
                ref, _, tag = e[1:].partition('{')
                out += self.reference(ctx, ref, tag.rstrip('}'), collect)
            elif any(c in e for c in '*?['):
                out += self.tree.glob(ctx.mod.dir, e)
            else:
                out.append(norm(f'{ctx.mod.dir}/{e}'))
        return out

    def source(self, ctx, path):
        if isinstance(path, tuple):  # already handled (generated)
            return
        ext = path.rsplit('.', 1)[-1]
        if ext == 'java':
            self.plan.add_java(path, ctx.bundle)
        elif ext == 'aidl':
            t = self.plan.task('aidl', f'aidl:{ctx.mod.name}', ctx.bundle,
                               files=[], flags=ctx.aidl_flags)
            t.args['files'].append(path)
            self.plan.aidl_includes[t.key].update(ctx.includes)
            self.plan.needed.add(path)
        elif ext == 'logtags':
            t = self.plan.task('logtags', f'logtags:{ctx.mod.name}', ctx.bundle, files=[])
            t.args['files'].append(path)
            self.plan.needed.add(path)
        elif ext == 'proto':
            proto = ctx.props.get('proto') or {}
            ctx.tasks.add(f'proto:{ctx.mod.name}')
            t = self.plan.task('proto', f'proto:{ctx.mod.name}', ctx.bundle, files=[],
                               type=proto.get('type', 'lite'),
                               includes=[norm(f'{ctx.mod.dir}/{d}') for d in as_list(proto.get('local_include_dirs'))]
                               + [norm(d) for d in as_list(proto.get('include_dirs'))],
                               canonical=proto.get('canonical_path_from_root', True),
                               output_params=as_list(proto.get('output_params')),
                               moddir=ctx.mod.dir)
            t.args['files'].append(path)
            self.plan.needed.add(path)
        elif ext in ('srcjar',):
            self.plan.unsupported[f'srcjar {path}'].add(ctx.mod.name)
        else:
            self.plan.needed.add(path)  # .txt and the like, used by genrules

    # --- references -----------------------------------------------------
    def reference(self, ctx, name, tag, collect):
        mod = self.modules.get(name)
        if mod is None:
            m = AIDL_VERSIONED.match(name)
            base = m and self.modules.get(m.group('name'))
            if base is not None and base.type == 'aidl_interface':
                if collect:
                    self.aidl_interface(base, self.modules.merged(base), m.group('version'),
                                        ctx.bundle, ctx.mod.name, as_sources=True)
                return []
            if collect:
                self.plan.missing[name].add(ctx.mod.name)
            return []
        props = self.modules.merged(mod)
        if mod.type == 'filegroup':
            sub = Context(mod, props, ctx.bundle, ctx.includes, ctx.aidl_flags, ctx.tasks)
            files = self.expand(sub, as_list(props.get('srcs')) + as_list(props.get('device_common_srcs')))
            excluded = set(self.expand(sub, as_list(props.get('exclude_srcs')), collect=False))
            files = [f for f in files if f not in excluded]
            if props.get('path') and collect:
                ctx.includes.append(norm(f'{mod.dir}/{props["path"]}'))
            return files
        if not collect:
            return []
        if mod.type == 'aidl_interface':
            self.aidl_interface(mod, props, None, ctx.bundle, ctx.mod.name, as_sources=True)
        elif mod.type == 'android_app' and name == 'framework-res':
            self.plan.task('framework-res', 'framework-res', ctx.bundle)
        elif mod.type == 'aconfig_declarations_group':
            self.aconfig_group(mod, props, ctx.bundle)
        elif mod.type in ('genrule', 'java_genrule', 'gensrcs'):
            return self.genrule(ctx, mod, props)
        elif mod.type == 'xsd_config':
            self.plan.task('xsd', f'xsd:{name}', ctx.bundle, src=norm(f'{mod.dir}/{as_list(props.get("srcs"))[0]}'),
                           package=props.get('package_name'), props=props, moddir=mod.dir)
            self.plan.needed.update(self.tree.dir_files(mod.dir))
        elif mod.type == 'java_system_features_srcs':
            self.plan.task('systemfeatures', f'systemfeatures:{name}', ctx.bundle,
                           cls=props.get('full_class_name'))
            self.plan.needed.add('frameworks/base/tools/systemfeatures/src/com/android/'
                                 'systemfeatures/SystemFeaturesGenerator.kt')
        elif mod.type in JAVA_LIB_TYPES:
            self.library(name, ctx.bundle, ctx.mod.name)
        else:
            self.plan.unsupported[f'{mod.type} {name}'].add(ctx.mod.name)
        return []

    def genrule(self, ctx, mod, props):
        cmd = props.get('cmd') or ''
        srcs = self.expand(Context(mod, props, ctx.bundle, ctx.includes, ctx.aidl_flags, ctx.tasks),
                           as_list(props.get('srcs')))
        # Copies (the release-selected and mock sources) are their inputs.
        if re.fullmatch(r'\s*(mkdir -p [^;]+;\s*)?cp \$\(in\) \$\(out\);?\s*', cmd):
            return [s for s in srcs if s.endswith('.java')]
        self.plan.needed.update(srcs)
        ctx.tasks.add(f'genrule:{mod.name}')
        self.plan.task('genrule', f'genrule:{mod.name}', ctx.bundle, module=mod.name,
                       type=mod.type, cmd=cmd, srcs=srcs, out=props.get('out'),
                       tools=as_list(props.get('tools')),
                       tool_files=[norm(f'{mod.dir}/{f}') for f in as_list(props.get('tool_files'))],
                       moddir=mod.dir, output_extension=props.get('output_extension'))
        self.plan.needed.update(norm(f'{mod.dir}/{f}') for f in as_list(props.get('tool_files')))
        return []

    def module_files(self, mod, props):
        ctx = Context(mod, props, False, [], [])
        return [f for f in self.expand(ctx, as_list(props.get('srcs'))) if isinstance(f, str)]

    # --- aconfig --------------------------------------------------------
    def aconfig(self, decl_name, bundle, referrer):
        mod = self.modules.get(decl_name) if decl_name else None
        if mod is None:
            self.plan.missing[decl_name].add(referrer)
            return
        props = self.modules.merged(mod)
        files = self.module_files(mod, props)
        t = self.plan.task('aconfig', 'aconfig', bundle, files=set())
        t.args['files'].update(files)
        self.plan.needed.update(files)

    def aconfig_group(self, mod, props, bundle):
        for lib in as_list(props.get('java_aconfig_libraries')):
            self.library(lib, bundle, mod.name)
        for group in as_list(props.get('aconfig_declarations_groups')):
            g = self.modules.get(group)
            if g is None:
                self.plan.missing[group].add(mod.name)
            else:
                self.aconfig_group(g, self.modules.merged(g), bundle)

    # --- sysprop --------------------------------------------------------
    def sysprop(self, mod, props, bundle):
        files = self.module_files(mod, props)
        self.plan.needed.update(files)
        self.plan.task('sysprop', f'sysprop:{mod.name}', bundle, files=files,
                       owner=props.get('property_owner'), name=mod.name)

    # --- AIDL interfaces --------------------------------------------------
    def aidl_versions(self, props):
        info = props.get('versions_with_info') or []
        versions = {str(v.get('version')): as_list(v.get('imports')) for v in info if isinstance(v, dict)}
        for v in as_list(props.get('versions')):
            versions.setdefault(v, as_list(props.get('imports')))
        return versions

    def aidl_interface(self, mod, props, version, bundle, referrer, as_sources=False, java=True):
        versions = self.aidl_versions(props)
        frozen = sorted(versions, key=int)
        latest = frozen[-1] if frozen else None
        nxt = str(int(latest) + 1) if latest else '1'
        unstable = bool(props.get('unstable'))
        if version is None:
            version = latest if (props.get('frozen') and latest) else (None if unstable else nxt)
        key = f'aidl-interface:{mod.name}-V{version}'
        if key in self.plan.tasks:
            t = self.plan.tasks[key]
            if java:
                t.bundle |= bundle
                t.args['java'] = True
            return key
        if version is not None and version in versions:
            root = f'{mod.dir}/aidl_api/{mod.name}/{version}'
            files = [f for f in self.tree.dir_files(root) if f.endswith('.aidl')]
            imports = versions[version]
            hash_file = f'{root}/.hash'
        else:
            local = props.get('local_include_dir')
            root = norm(f'{mod.dir}/{local}') if local else mod.dir
            ctx = Context(mod, props, bundle, [], [])
            files = [f for f in self.expand(ctx, as_list(props.get('srcs'))) if isinstance(f, str)]
            imports = as_list(props.get('imports'))
            hash_file = None
        # Soong passes the interface's own root with -N, and each import as a
        # preprocessed .aidl file (-p), so versions of one type never clash.
        includes = [norm(d) for d in as_list(props.get('include_dirs'))]
        for header in as_list(props.get('headers')):
            includes += self.aidl_library(header, mod.name)
        self.plan.include_dirs.update(includes)
        import_keys = []
        for imp in imports:
            k = self.aidl_import(imp, bundle, mod.name)
            if k:
                import_keys.append(k)
                includes += sorted(self.plan.aidl_includes.get(k, ()))
        self.plan.needed.update(files)
        if hash_file:
            self.plan.needed.add(hash_file)
        t = self.plan.task('aidl-interface', key, bundle, files=files, version=version,
                           structured=not unstable, hash_file=hash_file, name=mod.name,
                           stability=props.get('stability'), root=root, imports=import_keys,
                           flags=['-t'] if props.get('gen_trace') else [], java=java)
        if not java:
            t.bundle = False
        self.plan.aidl_includes[key].update(includes)
        java = (props.get('backend') or {}).get('java') or {}
        for lib in as_list(java.get('additional_libs')):
            self.library(lib, False, mod.name)
        return key

    def aidl_library(self, name, referrer):
        """An aidl_library's include directory (its headers' import root)."""
        mod = self.modules.get(name)
        if mod is None or mod.type != 'aidl_library':
            self.plan.missing[name].add(referrer)
            return []
        props = self.modules.merged(mod)
        self.plan.needed.update(self.module_files(mod, {'srcs': as_list(props.get('hdrs'))}))
        for dep in as_list(props.get('deps')):
            self.aidl_library(dep, referrer)
        return [norm(f'{mod.dir}/{props.get("strip_import_prefix", "")}')]

    def aidl_import(self, imp, bundle, referrer):
        m = re.match(r'^(.+?)(?:-V(\d+))?$', imp)
        name, version = m.group(1), m.group(2)
        mod = self.modules.get(name)
        if mod is None or mod.type != 'aidl_interface':
            self.plan.missing[imp].add(referrer)
            return None
        return self.aidl_interface(mod, self.modules.merged(mod), version, bundle, referrer,
                                   java=False)

    # --- HIDL -------------------------------------------------------------
    def hidl(self, mod, constants, bundle, referrer):
        key = f'hidl:{mod.name}{":constants" if constants else ""}'
        if key in self.plan.tasks:
            self.plan.tasks[key].bundle |= bundle
            return
        props = self.modules.merged(mod)
        files = self.module_files(mod, props)
        self.plan.needed.update(files)
        self.plan.task('hidl', key, bundle, fqname=mod.name, root=props.get('root'),
                       constants=constants)
        for dep in as_list(props.get('interfaces')):
            pkg, ver = dep.split('@')
            if not constants:
                self.library(f'{pkg}-V{ver}-java', bundle, mod.name)
            else:
                self.hidl_files(dep)

    def hidl_files(self, fqname, seen=None):
        seen = seen if seen is not None else set()
        mod = self.modules.get(fqname)
        if mod is None or fqname in seen:
            return
        seen.add(fqname)
        props = self.modules.merged(mod)
        self.plan.needed.update(self.module_files(mod, props))
        for dep in as_list(props.get('interfaces')):
            self.hidl_files(dep, seen)


class Context:
    def __init__(self, mod, props, bundle, includes, aidl_flags, tasks=None):
        self.mod, self.props, self.bundle = mod, props, bundle
        self.includes, self.aidl_flags = includes, aidl_flags
        self.tasks = tasks if tasks is not None else set()  # generation tasks of its srcs
