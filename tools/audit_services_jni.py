#!/usr/bin/env python3
"""Source inventory, not a semantic compatibility score or a live-app test.

Compare service Default subclasses with the exact supplied framework JAR,
enumerate registered service keys, and record JNI registration/stand-in rows.
Only javap runs; no Android app or system server is started.
"""
import argparse
import collections
import datetime
import hashlib
import json
import pathlib
import re
import subprocess


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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--framework-jar', type=pathlib.Path, required=True)
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

    classes = []
    method_re = re.compile(r'\b(?:public|protected)\s+(?:(?:static|final|synchronized|native)\s+)*'
                           r'([\w.$<>, ?\[\]]+?)\s+(\w+)\s*\(([^{};]*)\)\s*(?:throws[\w.,\s]+)?\{')
    for path in sorted((root / 'app/java').rglob('*.java')):
        text = path.read_text(encoding='utf8')
        code = mask(text)
        imports = {name.rsplit('.', 1)[-1]: name for name in re.findall(r'\bimport\s+([\w.]+);', text)}
        blocks = []
        for m in re.finditer(r'\b(?:class|interface|enum)\s+(\w+)([^{};]*)\{', code):
            blocks.append((m.start(), end_brace(code, m.end() - 1), m))
        for start, finish, decl in blocks:
            parent = re.search(r'\bextends\s+([\w.]+)\.(Default|Stub)\b', decl[2])
            if not parent:
                continue
            iface = parent[1]
            iface = imports.get(iface, iface)
            methods = []
            for method in method_re.finditer(code, decl.end(), finish):
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
            classes.append({'class': decl[1], 'interface': iface, 'base_kind': parent[2],
                            'source': ref(path, text, start), 'explicit_methods': methods})
        for inline in re.finditer(r'\bnew\s+([\w.]+)\.Default\s*\(\s*\)', code):
            iface = imports.get(inline[1], inline[1])
            classes.append({'class': '<direct Default instance>', 'interface': iface, 'base_kind': 'Default',
                            'source': ref(path, text, inline.start()), 'explicit_methods': []})

    interfaces = sorted({c['interface'] for c in classes if '.' in c['interface']})
    command = ['javap', '-public', '-classpath', str(args.framework_jar), *[i + '$Default' for i in interfaces]]
    inspected = subprocess.run(command, capture_output=True, text=True, encoding='utf8', errors='replace', check=False)
    (args.out.parent / 'services-default-javap.txt').write_text(inspected.stdout + inspected.stderr, encoding='utf8')
    current = None
    defaults = {}
    for line in inspected.stdout.splitlines():
        header = re.match(r'public class ([\w.$]+)\$Default\b', line)
        if header:
            current = header[1]
            defaults[current] = []
        method = re.match(r'\s+public (?:[\w.$<>?,\[\] ]+) ([\w$]+)\(([^)]*)\)', line)
        if method and current and method[1] != 'asBinder':
            defaults[current].append({'name': method[1], 'arity': arity(method[2]), 'signature': line.strip()})
    if not defaults:
        raise RuntimeError('No Java .class Default interfaces found; an installed DEX JAR is not a javap input')
    gaps = []
    for cls in classes:
        api = defaults.get(cls['interface'])
        cls['jar_interface_available'] = api is not None
        cls['interface_method_count'] = len(api) if api is not None else None
        explicit = {m['name'] for m in cls['explicit_methods']}
        cls['inherited_default_count'] = 0
        if api is None or cls['base_kind'] != 'Default':
            continue
        for method in api:
            if method['name'] in explicit:
                continue
            cls['inherited_default_count'] += 1
            gaps.append({'class': cls['class'], 'interface': cls['interface'], **method,
                         'status': 'missing_override_inherited_default', 'source': cls['source'],
                         'behavior_reference': {'path': 'app/java/com/gesher/server/StandIn.java', 'line': 145},
                         'signature_match_basis': 'interface method name absent from explicit class methods'})

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
        registration_rows.append({'symbol': symbol, 'aosp_source': ref(upstream, upstream_text, m.start()),
                                  'local_symbol_references': found,
                                  'status': 'registration_symbol_referenced_unverified' if found else 'upstream_registration_not_referenced_by_local_surface',
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
         'risk': 'Current source registers deviceidle with the entire IDeviceIdleController.Default. Parent confirmed installed PhoneDoctor constructor NPE from absent service. Source presence does not prove installation or semantics.',
         'evidence': anchor('app/java/com/gesher/server/Services.java', 'services.put("deviceidle"')},
        {'id': 'local-binder-injection', 'status': 'implemented_unverified', 'priority': 2,
         'family': 'Camera/accessibility app-local ServiceManager cache interception',
         'risk': 'Do not classify these as absent solely from central service keys. Cross-process, death/reconnect and per-method contracts still need explicit validation.',
         'evidence': [anchor('app/java/com/gesher/server/CameraService.java', 'public static void installInProcess()'),
                      anchor('app/java/com/gesher/app/accessibility/AccessibilityService.java', 'public static void installInProcess()')]},
        {'id': 'enterprise-shortcuts-usage', 'status': 'missing', 'priority': 2,
         'family': 'Device policy, shortcuts, usage stats and DropBox implementations',
         'risk': 'Empty Default subclasses are placeholders; source comments about device state are not additional implementation.',
         'classes': ['DevicePolicyService','ShortcutService','UsageStatsService','DropBoxManagerService'],
         'evidence': anchor('app/java/com/gesher/server/DeviceServices.java', 'class DevicePolicyService extends')},
        {'id': 'cellular-unsupported', 'status': 'intentional_default', 'priority': 3,
         'family': 'Cellular/SIM/subscription services',
         'risk': 'Telephony/phone subscriber/subscription/registry services use Default subclasses. A desktop without cellular hardware should report unavailable capabilities consistently; missing callback semantics remain separate.',
         'classes': ['TelephonyService','PhoneSubInfoService','SubscriptionService','TelephonyRegistryService'],
         'evidence': anchor('app/java/com/gesher/server/DeviceServices.java', 'class TelephonyService extends')},
        {'id': 'hardware-policy-vibration', 'status': 'intentional_default', 'priority': 3,
         'family': 'Vibrator/haptic hardware',
         'risk': 'No Android vibrator hardware; absence should be explicit rather than presented as a crash-fix gap.',
         'evidence': anchor('app/java/com/gesher/server/Services.java', '// A device without a vibrator')},
        {'id': 'settings-persistence', 'status': 'intentional_default', 'priority': 2,
         'family': 'SettingsProvider persistence and generations',
         'risk': 'Values are in-memory until server exit; no generation tracking. This differs from Android persistent settings.',
         'evidence': anchor('app/java/com/gesher/server/SettingsProvider.java', 'Values put are kept until the system')},
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
        reviewed.append({'id': 'jni-'+symbol.removeprefix('register_'), 'status': 'missing', 'priority': 2,
                         'family': symbol, 'risk': 'No local registration-symbol reference or selected source mention was found. Native class/API availability is separately audited; verify aliases before implementation.',
                         'evidence': row['aosp_source'], 'negative_search_scope': ['compat','cmake','app/java']})

    for finding in reviewed:
        evidence = finding['evidence']
        locations = evidence if isinstance(evidence, list) else [evidence]
        if any(item.get('status') == 'source_anchor_not_found' for item in locations):
            finding['proposed_status_before_source_check'] = finding['status']
            finding['status'] = 'not_assessed'
            finding['assessment_note'] = 'An expected source anchor is absent in this tree; the descriptive family claim was not re-established.'
        if finding.get('classes'):
            finding['inherited_default_methods'] = sum(c['inherited_default_count'] for c in classes if c['class'] in finding['classes'])

    result = {'schema_version': 1, 'date': '2026-11-05', 'aosp_tag': 'android-16.0.0_r4',
              'scope': 'JNI registration/stand-ins; app/java Binder Default services; direct Services/SystemServer registry keys. No user apps started.',
              'framework_jar': {'path': str(args.framework_jar.resolve()), 'sha256': hashlib.sha256(args.framework_jar.read_bytes()).hexdigest()},
              'framework_input_kind': 'Existing Java .class compile input; not the installed DEX JAR and not a new framework build receipt.',
              'methodology_limits': ['Source WIP and supplied framework JAR can differ from installed runtime.',
                  'Explicit implementations are unverified; source presence is not semantic coverage.',
                  'AIDL Default missing-method comparison uses method names, not full overload/type resolution.',
                  'Textual JNI symbol comparison is a candidate list, not proof every omitted registration is required.',
                  'Fallback JNI rows can be overwritten later and must not be counted as active noops without registration-order review.'],
              'service_classes': classes, 'inherited_default_methods': gaps, 'direct_service_registrations': registrations,
              'context_key_candidates': unregistered_constants, 'jni_upstream_registrations': registration_rows,
              'jni_standin_methods': standins, 'jni_standin_inputs': standin_inputs,
              'generator_sha256': hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
              'source_text_sha256': refs,
              'source_hash_encoding': 'UTF-8 text with universal newline normalization, exactly as parsed.',
              'generated_at_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'git_head': subprocess.run(['git','rev-parse','HEAD'],cwd=root,capture_output=True,text=True,check=True).stdout.strip(),
              'reviewed_known_gap_families': reviewed,
              'summary': {'service_classes': len(classes), 'interfaces_in_jar': len(defaults),
                          'explicit_service_methods': sum(len(c['explicit_methods']) for c in classes),
                          'inherited_default_methods': len(gaps), 'direct_service_registrations': len(registrations),
                          'context_key_candidates': len(unregistered_constants),
                          'upstream_jni_registration_rows': len(registration_rows),
                          'jni_registration_statuses': dict(collections.Counter(r['status'] for r in registration_rows)),
                          'jni_standin_methods': len(standins),
                          'explicit_method_statuses': dict(collections.Counter(m['status'] for c in classes for m in c['explicit_methods']))}}
    args.out.write_text(json.dumps(result, indent=2, ensure_ascii=False)+'\n', encoding='utf8')
    print(json.dumps(result['summary'], indent=2))


if __name__ == '__main__':
    main()
