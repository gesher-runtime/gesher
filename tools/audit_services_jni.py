#!/usr/bin/env python3
"""Source inventory, not a semantic compatibility score or a live-app test.

Compare service Default subclasses with the exact supplied framework/app classes,
enumerate registered service keys, and record JNI registration/stand-in rows.
Class files are parsed without loading them; javap only reads Context constants.
No Android app or system server is started.
"""
import argparse
import collections
import datetime
import hashlib
import json
import pathlib
import re
import subprocess

import java_member_inventory as members


# Reviewed alternate names in android_runtime.cc, not a heuristic name match.
REGISTRATION_ALIASES = {
    'register_android_net_LocalSocketImpl': 'register_gesher_LocalSocketImpl',
    'register_android_util_MemoryIntArray': 'register_gesher_MemoryIntArray',
    'register_android_os_MemoryFile': 'register_gesher_MemoryFile',
    'register_android_security_Scrypt': 'register_gesher_Scrypt',
}


def mask(text):
    return re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), text)


def end_brace(code, start):
    depth = 0
    for i in range(start, len(code)):
        if code[i] == '{':
            depth += 1
        elif code[i] == '}':
            depth -= 1
            if not depth:
                return i
    raise ValueError('unbalanced source braces')


def arity(args):
    if not args.strip():
        return 0
    depth = 0
    result = 1
    for char in args:
        if char in '<([':
            depth += 1
        elif char in '>)]':
            depth -= 1
        elif char == ',' and not depth:
            result += 1
    return result


def service_base(name, classes, framework):
    """Find the actual compiled AIDL ancestor, including indirect subclasses."""
    seen = set()
    while name and name != 'java.lang.Object':
        if name in seen:
            raise ValueError('cyclic service class hierarchy')
        seen.add(name)
        cls = classes.get(name)
        if cls is None:
            return None
        iface, separator, kind = name.rpartition('$')
        if (separator and kind in ('Default', 'Stub') and name in framework
                and iface in framework and framework[iface].flags & members.INTERFACE):
            return iface, kind
        name = cls.superclass
    return None


def service_surface(name, classes):
    """Collect public instance descriptors, retaining incomplete hierarchies."""
    methods, missing, seen, pending = {}, set(), set(), [name]
    while pending:
        owner = pending.pop()
        if owner in seen or owner == 'java.lang.Object':
            continue
        seen.add(owner)
        cls = classes.get(owner)
        if cls is None:
            missing.add(owner)
            continue
        for method in cls.members.values():
            if (method.kind == 'method' and method.name not in ('<init>', '<clinit>', 'asBinder')
                    and method.flags & members.PUBLIC and not method.flags & members.STATIC):
                methods.setdefault(method.key, method)
        pending.extend(cls.interfaces)
        if cls.superclass:
            pending.append(cls.superclass)
    return methods, sorted(missing)


def compare_services(source_classes, framework, app, declarations=None):
    """Resolve exact descriptors from bytes, never from Java source spellings.

    Framework classes take precedence over app classes, as on the parent-first
    service class path. Covariant/generic overrides are represented by the
    compiler's bridge methods; a name or parameter-count match is insufficient.
    Missing artifacts or ancestry are unmeasured, not a zero-gap result.
    """
    declarations = declarations or {}
    available = {**(app or {}), **framework}
    resolver = members.Resolver(available)
    classes = [dict(item) for item in source_classes]
    known = {item.get('binary_class') for item in classes}
    if app is not None:
        for name in sorted(app):
            base = service_base(name, available, framework)
            if base is None or name in known or name == base[0] + '$' + base[1]:
                continue
            source = declarations.get(name, {})
            classes.append({'class': source.get('class', name.rsplit('$', 1)[-1].rsplit('.', 1)[-1]),
                            'binary_class': name, 'interface': base[0], 'base_kind': base[1],
                            'source': source.get('source'), 'explicit_methods': [],
                            'discovery_basis': 'compiled_class_ancestry'})
    gaps = []
    for cls in classes:
        name = cls.get('binary_class')
        base = service_base(name, available, framework) if name in available else None
        if base:
            cls['source_declared_interface'] = cls['interface']
            cls['interface'], cls['base_kind'] = base
        default = cls['interface'] + '$Default'
        cls['jar_interface_available'] = default in framework
        cls['inherited_default_count'] = None
        cls['method_resolutions'] = []
        cls['interface_method_count'] = None
        if default not in framework:
            cls['comparison_status'] = 'framework_default_class_unavailable'
            continue
        api, missing = service_surface(default, framework)
        cls['interface_method_count'] = len(api) if not missing else None
        cls['observed_interface_method_count'] = len(api)
        cls['unavailable_surface_classes'] = missing
        if app is None:
            cls['comparison_status'] = 'app_class_input_not_provided'
            continue
        if name not in available:
            cls['comparison_status'] = 'compiled_service_class_unavailable'
            continue
        if not base or base != (cls['interface'], cls['base_kind']):
            cls['comparison_status'] = 'compiled_service_ancestry_unresolved'
            continue
        unresolved, inherited = bool(missing), 0
        for key, expected in sorted(api.items()):
            found, unknown = resolver.lookup(name, key)
            original, original_unknown = resolver.lookup(default, key)
            row = {'name': expected.name, 'descriptor': expected.descriptor,
                   'signature': expected.name + expected.descriptor,
                   'resolved_owners': [owner for owner, _ in found],
                   'resolved_providers': ['framework' if owner in framework else 'app' for owner, _ in found],
                   'descriptor_types_not_in_inputs': sorted(members.descriptor_classes(expected.descriptor) - available.keys()),
                   'signature_match_basis': 'compiled JVM method name and complete descriptor (parameters and return type)'}
            if unknown or original_unknown or len(found) > 1:
                row['status'] = 'compiled_method_resolution_unresolved'
                unresolved = True
            elif not found:
                row['status'] = 'compiled_method_unavailable'
                unresolved = True
            elif found[0][1].flags & (members.STATIC | members.ABSTRACT) or not found[0][1].flags & members.PUBLIC:
                row['status'] = 'compiled_method_not_concrete_public_instance'
                unresolved = True
            elif cls['base_kind'] == 'Default' and found == original:
                row['status'] = 'missing_override_inherited_default'
                inherited += 1
                gaps.append({'class': cls['class'], 'binary_class': name, 'interface': cls['interface'],
                             **row, 'source': cls['source'],
                             'behavior_reference': {'path': 'app/java/com/gesher/server/StandIn.java', 'line': 145}})
            else:
                row['status'] = 'compiled_implementation_unverified'
                row['implementation_location'] = 'declared' if found[0][0] == name else 'inherited'
                row['compiler_bridge'] = bool(found[0][1].flags & 0x40)
            cls['method_resolutions'].append(row)
        cls['observed_inherited_default_count'] = inherited
        cls['comparison_status'] = 'unresolved_compiled_hierarchy' if unresolved else 'measured_compiled_descriptors'
        if not unresolved:
            cls['inherited_default_count'] = inherited
    return classes, gaps


def jni_finding(row):
    """Keep source references distinct from absence and runtime acceptance."""
    found = list(row['local_symbol_references'])
    for alias in row.get('local_registration_aliases', []):
        found.extend(alias['source_references'])
    result = {
        'id': 'jni-' + row['symbol'].removeprefix('register_'),
        'status': 'implemented_unverified' if found else 'missing',
        'priority': 2, 'family': row['symbol'],
        'risk': ('Local registration-symbol references exist; reachability, registration order and '
                 'behavior require separate build/runtime evidence.' if found else
                 'No local registration-symbol reference was found. Verify aliases and alternate '
                 'implementations before treating this as a runtime gap.'),
        'evidence': found if found else row['aosp_source'],
    }
    if not found:
        result['negative_search_scope'] = ['compat/**/*.cc', 'compat/**/*.cpp', 'compat/**/*.h']
    return result


def source_service_classes(root, ref):
    classes, declarations = [], {}
    method_re = re.compile(r'\b(?:public|protected)\s+(?:(?:static|final|synchronized|native)\s+)*'
                           r'([\w.$<>, ?\[\]]+?)\s+(\w+)\s*\(([^{};]*)\)\s*(?:throws[\w.,\s]+)?\{')
    for path in sorted((root / 'app/java').rglob('*.java')):
        text = path.read_text(encoding='utf8')
        code = mask(text)
        depth, depths = 0, [0]
        for char in code:
            depth += (char == '{') - (char == '}')
            depths.append(depth)
        package = re.search(r'\bpackage\s+([\w.]+)\s*;', code)
        package = package[1] + '.' if package else ''
        imports = {name.rsplit('.', 1)[-1]: name for name in re.findall(r'\bimport\s+([\w.]+);', text)}
        blocks = []
        for m in re.finditer(r'\b(?:class|interface|enum)\s+(\w+)([^{};]*)\{', code):
            blocks.append((m.start(), end_brace(code, m.end() - 1), m))
        for start, finish, decl in blocks:
            enclosing = [block[2][1] for block in blocks if block[0] < start < block[1]]
            binary = package + '$'.join([*enclosing, decl[1]])
            declarations[binary] = {'class': decl[1], 'source': ref(path, text, start)}
            parent = re.search(r'\bextends\s+([\w.]+)\.(Default|Stub)\b', decl[2])
            if not parent:
                continue
            iface = parent[1]
            iface = imports.get(iface, iface)
            methods = []
            for method in method_re.finditer(code, decl.end(), finish):
                # A named-class owner alone is insufficient: anonymous objects
                # inside a method or initializer have their own method bodies.
                if depths[method.start()] != depths[decl.end()]:
                    continue
                owner = min((b for b in blocks if b[0] <= method.start() < b[1]), key=lambda b: b[1] - b[0])
                if owner[0] != start:
                    continue
                stop = end_brace(code, method.end() - 1)
                body = code[method.end():stop].strip()
                compact = re.sub(r'\s+', ' ', body)
                raw_body = text[method.end():stop].strip()
                if not compact:
                    status = 'explicit_noop'
                elif re.fullmatch(r'return (?:null|false|true|0|0L|-1|-1L);', compact):
                    status = 'explicit_default_answer'
                elif re.fullmatch(r'return (?:\w+\.)*(?:emptyList|emptyMap|emptySet)\(\);', compact):
                    status = 'explicit_empty_collection'
                elif 'throw new UnsupportedOperationException' in compact:
                    status = 'explicit_unsupported_path'
                else:
                    status = 'implemented_unverified'
                methods.append({'name': method[2], 'arity': arity(method[3]), 'return_type_source': method[1].strip(),
                                'status': status, 'source': ref(path, text, method.start()),
                                'body_sha256': hashlib.sha256(raw_body.encode()).hexdigest()})
            classes.append({'class': decl[1], 'binary_class': binary,
                            'interface': iface, 'base_kind': parent[2],
                            'source': ref(path, text, start), 'explicit_methods': methods})
        for inline in re.finditer(r'\bnew\s+([\w.]+)\.Default\s*\(\s*\)', code):
            # Anonymous overrides have their own compiler-assigned class. They
            # are discovered through compiled ancestry, not treated as a bare Default.
            if code[inline.end():].lstrip().startswith('{'):
                continue
            iface = imports.get(inline[1], inline[1])
            classes.append({'class': '<direct Default instance>', 'interface': iface, 'base_kind': 'Default',
                            'binary_class': iface + '$Default', 'direct_default_instance': True,
                            'source': ref(path, text, inline.start()), 'explicit_methods': []})

    return classes, declarations


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--framework-jar', type=pathlib.Path, required=True)
    ap.add_argument('--app-classes', type=pathlib.Path,
                    help='Built app Java .class ZIP/JAR, not DEX. Without it, service overrides are unmeasured.')
    ap.add_argument('--aosp-dir', type=pathlib.Path, required=True)
    ap.add_argument('--out', type=pathlib.Path, required=True)
    args = ap.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    refs = {}

    def ref(path, text, offset):
        path = path.resolve()
        label = path.relative_to(root).as_posix() if path.is_relative_to(root) else str(path)
        # Hash the exact normalized source text used for this row, rather than
        # re-reading a file another agent may be editing during the audit.
        digest = hashlib.sha256(text.encode('utf8')).hexdigest()
        if label in refs and refs[label] != digest:
            raise RuntimeError('Source changed during audit: ' + label)
        refs[label] = digest
        return {'path': label, 'line': text.count('\n', 0, offset) + 1}

    classes, declarations = source_service_classes(root, ref)

    framework, framework_input = members.archive_members(args.framework_jar, 'sdk', 'framework-classes')
    framework_input['role'] = 'framework_compile_input'
    app, app_input = None, {'status': 'input_not_provided'}
    if args.app_classes is not None:
        app, app_input = members.archive_members(args.app_classes, 'sdk', 'app-classes')
        app_input.update(status='class_bytes_inspected', role='app_compile_input', path=str(args.app_classes.resolve()))
    classes, gaps = compare_services(classes, framework, app, declarations)
    defaults = {c['interface'] for c in classes if c['jar_interface_available']}

    context = subprocess.run(['javap', '-constants', '-classpath', str(args.framework_jar), 'android.content.Context'],
                             capture_output=True, text=True, encoding='utf8', errors='replace')
    service_constants = {m[1]: m[2] for m in re.finditer(r'public static final java.lang.String (\w+_SERVICE) = "([^"]+)";', context.stdout)}
    registrations = []
    for filename in ('Services.java', 'SystemServer.java'):
        path = root / 'app/java/com/gesher/server' / filename
        text = path.read_text(encoding='utf8')
        for m in re.finditer(r'\bservices\.put\(\s*([^,\n]+?)\s*,', text):
            expr=m[1].strip()
            key = service_constants.get(expr.split('.',1)[1]) if expr.startswith('Context.') else expr.strip('"') if expr.startswith('"') else None
            registrations.append({'key_expression': m[1], 'resolved_key': key, 'status': 'registered_source_unverified',
                                  'source': ref(path, text, m.start()),
                                  'expression_excerpt': re.sub(r'\s+', ' ', text[m.end():m.end()+180]).split(';')[0]})
    registered = {r['resolved_key'] for r in registrations if r['resolved_key']}
    unregistered_constants = [{'constant': name, 'key': key, 'status': 'context_key_without_direct_source_registration',
                               'caveat': 'Some Context services are local managers or use different Binder keys; candidate only.'}
                              for name, key in service_constants.items() if key not in registered]

    upstream = args.aosp_dir / 'frameworks/base/core/jni/AndroidRuntime.cpp'
    upstream_text = upstream.read_text(encoding='utf8')
    local_symbols = collections.defaultdict(list)
    for path in (root/'compat').rglob('*'):
        if path.suffix not in ('.cc','.cpp','.h'):
            continue
        text = path.read_text(encoding='utf8', errors='replace')
        for use in re.finditer(r'\bregister_\w+\b', mask(text)):
            local_symbols[use[0]].append(ref(path, text, use.start()))
    registration_rows = []
    for m in re.finditer(r'REG_JNI\((register_\w+)\)', upstream_text):
        symbol = m[1]
        found = local_symbols.get(symbol, [])
        alias = REGISTRATION_ALIASES.get(symbol)
        aliases = ([{'symbol': alias, 'source_references': local_symbols[alias]}]
                   if alias and local_symbols.get(alias) else [])
        registration_rows.append({'symbol': symbol, 'aosp_source': ref(upstream, upstream_text, m.start()),
                                  'local_symbol_references': found,
                                  'local_registration_aliases': aliases,
                                  'status': ('registration_symbol_referenced_unverified' if found else
                                             'alternate_registration_symbol_referenced_unverified' if aliases else
                                             'upstream_registration_not_referenced_by_local_surface'),
                                  'caveat': 'Textual registration-symbol comparison, not a reachable-call graph; aliases and alternate implementations need review.'})

    standins = []
    standin_inputs = []
    for rel, kind in [('compat/framework/attach_standins.cc','fallback_attach_may_be_replaced'),
                      ('out/native-build/build/gen/device_standins_tables.cc','generated_device_standin')]:
        path = root / rel
        if not path.exists():
            standin_inputs.append({'path': rel, 'kind': kind, 'status': 'input_not_available'})
            continue
        text = path.read_text(encoding='utf8')
        standin_inputs.append({'path': rel, 'kind': kind, 'status': 'source_inspected',
                               'source_text_sha256': hashlib.sha256(text.encode('utf8')).hexdigest()})
        mapping = {m[2]: m[1] for m in re.finditer(r'\{"([\w/$]+)",\s*(k\w+),', text)}
        for table in re.finditer(r'JNINativeMethod\s+(k\w+)\[\]\s*=\s*\{', text):
            finish = text.find('\n};', table.end())
            for entry in re.finditer(r'\{"(\w+)",\s*"([^"]+)",\s*([^\n}]+)', text[table.end():finish]):
                target = entry[3].strip().rstrip(',')
                zero = bool(re.search(r'(?:Standin_|gesher_device_Standin_|&)(?:zero|zeroFloat)\b', target))
                standins.append({'class': mapping.get(table[1]), 'method': entry[1], 'jni_signature': entry[2],
                                 'target': target, 'kind': kind,
                                 'status': 'default_zero_noop_standin' if zero else 'special_standin_unverified',
                                 'source': ref(path, text, table.end() + entry.start()),
                                 'caveat': 'Attach tables register first; later real registrations can replace them. Presence here alone is not an active gap.'})

    def anchor(rel, needle):
        path = root / rel
        text = path.read_text(encoding='utf8')
        offset = text.find(needle)
        if offset < 0:
            location = ref(path, text, 0)
            location.update(line=None, status='source_anchor_not_found')
            return location
        location = ref(path, text, offset)
        location['status'] = 'source_anchor_found'
        return location

    reviewed = [
        {'id': 'binder-default-dispatch', 'status': 'missing', 'priority': 1,
         'family': 'Unimplemented Binder methods return Default or throw only under strict-services',
         'risk': 'A void callback registration/request can silently do nothing; null object results can violate manager assumptions. Each contract needs separate review.',
         'evidence': anchor('app/java/com/gesher/server/StandIn.java', 'if (m.getDeclaringClass() == mDefault')},
        {'id': 'core-services-partial', 'status': 'missing', 'priority': 1,
         'family': 'Activity/package/window/notification/audio Default methods',
         'risk': 'Lifecycle, channel/group queries, policy, routing, observers and callbacks extend far beyond implemented startup paths.',
         'classes': ['ActivityManagerService','ActivityTaskManagerService','PackageManagerService','WindowManagerService','NotificationManagerService','AudioService'],
         'evidence': anchor('app/java/com/gesher/server/ActivityManagerService.java', 'extends IActivityManager.Default')},
        {'id': 'settings-intents', 'status': 'missing', 'priority': 1,
         'family': 'Built-in Android Settings activities beyond notification fallback',
         'risk': 'No built-in generic Settings activity. Unresolved battery exemption/app-details/all-files and other Settings intents depend on an installed handler; unresolved starts propagate START_INTENT_NOT_RESOLVED.',
         'evidence': anchor('app/java/com/gesher/server/ActivityTaskManagerService.java', 'SystemServer.log("startActivity: nothing handles')},
        {'id': 'document-uri-contract', 'status': 'implemented_unverified', 'priority': 1,
         'family': 'OPEN_DOCUMENT/GET_CONTENT/OPEN_DOCUMENT_TREE fallback',
         'risk': 'Windows picker returns file:// with grant flags; that is not proof of DocumentsProvider document IDs, persisted tree grants, MIME/multi-select or full SAF semantics.',
         'evidence': anchor('app/java/com/gesher/server/ActivityTaskManagerService.java', 'Uri uri = Uri.fromFile')},
        {'id': 'chooser-contract', 'status': 'intentional_default', 'priority': 2,
         'family': 'ACTION_CHOOSER',
         'risk': 'Unwraps and launches target; there is no Android chooser UI or complete target-ranking/share-sheet contract.',
         'evidence': anchor('app/java/com/gesher/server/ActivityTaskManagerService.java', '// ACTION_CHOOSER')},
        {'id': 'deviceidle-source-installed-skew', 'status': 'source_installed_skew', 'priority': 1,
         'family': 'DeviceIdleManager constructor and battery-exemption queries',
         'risk': 'Current source registers a desktop DeviceIdleController. The installed PhoneDoctor constructor NPE was from an absent service. Candidate source presence does not prove installation or complete device-idle semantics.',
         'evidence': anchor('app/java/com/gesher/server/Services.java', 'services.put("deviceidle"')},
        {'id': 'local-binder-injection', 'status': 'implemented_unverified', 'priority': 2,
         'family': 'Camera/accessibility app-local ServiceManager cache interception',
         'risk': 'Do not classify these as absent solely from central service keys. Cross-process, death/reconnect and per-method contracts still need explicit validation.',
         'evidence': [anchor('app/java/com/gesher/server/CameraService.java', 'public static void installInProcess()'),
                      anchor('app/java/com/gesher/app/accessibility/AccessibilityService.java', 'public static void installInProcess()')]},
        {'id': 'enterprise-shortcuts-usage', 'status': 'implemented_unverified', 'priority': 2,
         'family': 'Device policy, shortcuts, usage stats and DropBox implementations',
         'risk': 'Desktop policy, shortcut and usage implementations provide selected contracts; inherited Default methods and the DropBox placeholder remain separately unverified.',
         'classes': ['UnmanagedDevicePolicyService','DesktopShortcutService','DesktopUsageStatsService','DropBoxManagerService'],
         'evidence': [anchor('app/java/com/gesher/server/UnmanagedDevicePolicyService.java', 'extends IDevicePolicyManager.Default'),
                      anchor('app/java/com/gesher/server/DesktopShortcutService.java', 'extends IShortcutService.Default'),
                      anchor('app/java/com/gesher/server/DesktopUsageStatsService.java', 'extends IUsageStatsManager.Default')]},
        {'id': 'cellular-unsupported', 'status': 'intentional_default', 'priority': 3,
         'family': 'Cellular/SIM/subscription services',
         'risk': 'Telephony/phone subscriber/subscription/registry services use Default subclasses. A desktop without cellular hardware should report unavailable capabilities consistently; missing callback semantics remain separate.',
         'classes': ['TelephonyService','PhoneSubInfoService','SubscriptionService','TelephonyRegistryService'],
         'evidence': anchor('app/java/com/gesher/server/DeviceServices.java', 'class TelephonyService extends')},
        {'id': 'hardware-policy-vibration', 'status': 'intentional_default', 'priority': 3,
         'family': 'Vibrator/haptic hardware',
         'risk': 'No Android vibrator hardware; absence should be explicit rather than presented as a crash-fix gap.',
         'evidence': anchor('app/java/com/gesher/server/Services.java', '// A device without a vibrator')},
        {'id': 'settings-persistence', 'status': 'partial_implementation', 'priority': 2,
         'family': 'SettingsProvider persistence and generations',
         'risk': 'Provider rows are durable, but Android generation tracking, per-user state and restore metadata remain incomplete.',
         'evidence': anchor('app/java/com/gesher/server/SettingsProvider.java', 'No generation tracking')},
        {'id': 'jni-fallback-tables', 'status': 'intentional_default', 'priority': 2,
         'family': 'Generated device/attach native fallbacks',
         'risk': 'Many zero/noop entries exist but later real registrations replace some; registration order must be resolved before claiming an active native gap.',
         'evidence': anchor('compat/framework/attach_standins.cc', '// runs before the graphics registrations')},
        {'id': 'jni-optional-class-skip', 'status': 'implemented_unverified', 'priority': 2,
         'family': 'Conditional JNI registration by class availability',
         'risk': 'Missing registration prerequisite class is skipped after clearing exception. Loading the module successfully does not prove every native is registered.',
         'evidence': anchor('compat/framework/android_runtime.cc', 'for (const auto& r : android::gRegJNI)')},
    ]
    for symbol in ('register_android_net_LocalSocketImpl','register_android_util_MemoryIntArray',
                   'register_android_os_MemoryFile','register_android_security_Scrypt'):
        row = next(r for r in registration_rows if r['symbol'] == symbol)
        reviewed.append(jni_finding(row))

    for finding in reviewed:
        evidence = finding['evidence']
        locations = evidence if isinstance(evidence, list) else [evidence]
        if any(item.get('status') == 'source_anchor_not_found' for item in locations):
            finding['proposed_status_before_source_check'] = finding['status']
            finding['status'] = 'not_assessed'
            finding['assessment_note'] = 'An expected source anchor is absent in this tree; the descriptive family claim was not re-established.'
        if finding.get('classes'):
            selected = [c for c in classes if c['class'] in finding['classes']]
            finding['inherited_default_methods'] = (sum(c['inherited_default_count'] for c in selected)
                if selected and all(c['inherited_default_count'] is not None for c in selected) else None)

    unmeasured = sum(c['inherited_default_count'] is None for c in classes)
    result = {'schema_version': 2, 'date': datetime.datetime.now(datetime.timezone.utc).date().isoformat(), 'aosp_tag': 'android-16.0.0_r4',
              'scope': 'JNI registration/stand-ins; app/java Binder Default services; direct Services/SystemServer registry keys. No user apps started.',
              'framework_jar': {**framework_input, 'path': str(args.framework_jar.resolve())},
              'app_classes': app_input,
              'compiled_class_path': {'lookup_order': ['framework-classes', 'app-classes'],
                  'policy': 'Parent-first compile-input inventory; installed loader/provider selection is not measured.',
                  'app_classes_shadowed_by_framework': sorted(framework.keys() & (app or {}).keys())},
              'framework_input_kind': 'Existing Java .class compile input; not the installed DEX JAR and not a new framework build receipt.',
              'methodology_limits': ['Source WIP and supplied framework JAR can differ from installed runtime.',
                  'Explicit implementations are unverified; source presence is not semantic coverage.',
                  'AIDL methods use compiled JVM names and full descriptors, including compiler bridges and inherited implementations.',
                  'Compiled app/source correspondence requires a build receipt; input hashes alone do not establish a fresh build.',
                  'Missing app classes or hierarchy leave counts null; observed rows are a lower bound, not a completed inventory.',
                  'Descriptor types outside the supplied compile inputs are listed separately; this does not prove runtime absence.',
                  'Textual JNI symbol comparison is a candidate list, not proof every omitted registration is required.',
                  'Fallback JNI rows can be overwritten later and must not be counted as active noops without registration-order review.'],
              'service_classes': classes, 'inherited_default_methods': gaps, 'direct_service_registrations': registrations,
              'context_key_candidates': unregistered_constants, 'jni_upstream_registrations': registration_rows,
              'jni_standin_methods': standins, 'jni_standin_inputs': standin_inputs,
              'generator_sha256': hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
              'parser_source_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in (pathlib.Path(members.__file__), pathlib.Path(members.base.__file__))},
              'source_text_sha256': refs,
              'source_hash_encoding': 'UTF-8 text with universal newline normalization, exactly as parsed.',
              'generated_at_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'git_head': subprocess.run(['git','rev-parse','HEAD'],cwd=root,capture_output=True,text=True,check=True).stdout.strip(),
              'reviewed_known_gap_families': reviewed,
              'summary': {'service_classes': len(classes), 'interfaces_in_jar': len(defaults),
                          'explicit_service_methods': sum(len(c['explicit_methods']) for c in classes),
                          'inherited_default_methods': len(gaps) if app is not None and not unmeasured else None,
                          'observed_inherited_default_methods': len(gaps),
                          'unmeasured_service_classes': unmeasured,
                          'service_comparison_statuses': dict(collections.Counter(c['comparison_status'] for c in classes)),
                          'direct_service_registrations': len(registrations),
                          'context_key_candidates': len(unregistered_constants),
                          'upstream_jni_registration_rows': len(registration_rows),
                          'jni_registration_statuses': dict(collections.Counter(r['status'] for r in registration_rows)),
                          'jni_standin_methods': len(standins),
                          'explicit_method_statuses': dict(collections.Counter(m['status'] for c in classes for m in c['explicit_methods']))}}
    args.out.write_text(json.dumps(result, indent=2, ensure_ascii=False)+'\n', encoding='utf8')
    print(json.dumps(result['summary'], indent=2))


if __name__ == '__main__':
    main()
