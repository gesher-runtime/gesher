#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Verify pinned subscription imports and six selected SDK declarations in actual DEX inputs.

Static provider measurements are distinct from runtime, native, permission and
behavior qualification. The official SDK supplies declarations only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

import java_member_inventory as members
from nsd_subscription_imports import sources

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'android-jar', 'framework-dex', 'application-dex',
                 'application-classes', 'core-classes', 'upstream-sources', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    provenance = json.loads((ROOT / 'docs/research/nsd-subscription-core-sources.json').read_text())
    for name, expected in provenance['inputs'].items():
        if sha(args.upstream_sources / name) != expected['sha256']:
            raise ValueError('Pinned upstream input differs: ' + name)
    for path_key, hash_key in (('transform', 'transform_sha256'), ('block_helper', 'block_helper_sha256')):
        if sha(ROOT / provenance[path_key]) != provenance[hash_key]:
            raise ValueError('Source reconstruction helper differs: ' + path_key)
    reproduced = sources(args.upstream_sources)
    for name, text in reproduced.items():
        actual = ROOT / 'app/java/com/gesher/net/mdns' / name
        if actual.read_bytes() != text.encode() or sha(actual) != provenance['outputs'][name]['sha256']:
            raise ValueError('Undeclared imported source adaptation: ' + name)
    framework, framework_manifest = members.archive_members(args.framework_dex, 'boot', 'selected-framework')
    application, application_manifest = members.archive_members(args.application_dex, 'boot', 'fresh-application')
    classes = {}
    with zipfile.ZipFile(args.application_classes) as archive:
        for entry in archive.infolist():
            if not entry.filename.endswith('.class') or entry.filename.startswith('META-INF/'):
                continue
            parsed = members.class_members(archive.read(entry))
            if parsed.name in classes:
                raise ValueError('Duplicate compiled application class: ' + parsed.name)
            classes[parsed.name] = parsed
    classes_manifest = dict(path=str(args.application_classes), role='compile-only-application-classfiles',
                            sha256=sha(args.application_classes), class_definitions=len(classes))
    selected = []
    with zipfile.ZipFile(args.android_jar) as sdk:
        for owner in ('android.net.nsd.NsdManager', 'android.net.nsd.NsdManager$ServiceInfoCallback'):
            specified = members.class_members(sdk.read(owner.replace('.', '/') + '.class'))
            if owner in framework or owner not in application:
                raise ValueError('Selected runtime defining provider differs: ' + owner)
            for item in specified.members.values():
                if item.kind != 'method' or not item.flags & members.PUBLIC or item.name.startswith('<'):
                    continue
                if owner.endswith('NsdManager') and item.name not in ('registerServiceInfoCallback', 'unregisterServiceInfoCallback'):
                    continue
                actual = application[owner].members.get(item.key)
                compiled = classes[owner].members.get(item.key)
                if actual is None or compiled is None or members.flag_issues(item, actual) or members.flag_issues(item, compiled):
                    raise ValueError('Selected declaration differs: ' + repr((owner, item.key)))
                selected.append(dict(owner=owner, name=item.name, descriptor=item.descriptor, sdk_flags=item.flags,
                                     dex_flags=actual.flags, compile_flags=compiled.flags, provider='fresh-application-dex'))
    if len(selected) != 6:
        raise ValueError('Selected six-declaration denominator changed')
    native_path = ROOT / 'compat/net/windows_nsd_resolve.cc'; native_text = native_path.read_text()
    table = re.search(r'const JNINativeMethod kSubscriptionMethods\[\] = \{(.*?)\n\};', native_text, re.S)
    if table is None:
        raise ValueError('Subscriber native registration table absent')
    native_rows = re.findall(r'\{"(nativeSubscription\w+)", "([^"]+)", NATIVEHELPER_SYSV_JNI_ENTRY\((\w+)\)\}', table.group(1))
    declared = {item.name: item.descriptor for item in classes['com.gesher.net.WindowsNsdResolver'].members.values()
                if item.flags & members.NATIVE and item.flags & members.STATIC and item.name.startswith('nativeSubscription')}
    if len(native_rows) != 8 or {name: descriptor for name, descriptor, _ in native_rows} != declared:
        raise ValueError('Subscriber JNI binding differs from genuine compiled declaration')
    core = {}
    for path in args.core_classes.rglob('*.class'):
        if path.name == 'module-info.class':
            continue
        value = members.class_members(path.read_bytes()); core[value.name] = value
    closure = sorted(name for name in classes if name.startswith((
        'com.gesher.net.INsdSubscription', 'com.gesher.net.NsdSubscriptionClient',
        'com.gesher.net.WindowsNsdSubscriber', 'com.gesher.net.WindowsNsdResolver',
        'com.gesher.server.NsdSubscriptionService', 'com.gesher.net.mdns.', 'android.net.nsd.NsdManager')))
    missing = sorted(set(closure) - application.keys())
    if missing:
        raise ValueError('Fresh DEX lost subscription dependency definitions: ' + repr(missing))
    resolver = members.Resolver({**core, **framework, **application})
    command = [str(args.java_home / 'bin/javap'), '-verbose', '-classpath', str(args.application_classes), *closure]
    bytecode = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout
    references = set()
    for kind, owner, name, descriptor in re.findall(
            r'\s*#\d+ = (Fieldref|Methodref|InterfaceMethodref).*?// ([\w/$]+)\.([^:]+):([^\r\n]+)', bytecode):
        owner = owner.replace('/', '.')
        if owner.startswith(('android.', 'com.android.', 'com.gesher.')):
            references.add((owner, 'field' if kind == 'Fieldref' else 'method', name.strip('"'), descriptor.strip()))
    resolved, unresolved = [], []
    for owner, kind, name, descriptor in sorted(references):
        if owner not in resolver.classes:
            unresolved.append([owner, kind, name, descriptor, 'owner absent']); continue
        found, uncertain = resolver.lookup(owner, (kind, name, descriptor))
        if len(found) != 1 or uncertain:
            unresolved.append([owner, kind, name, descriptor, 'missing/ambiguous/incomplete hierarchy']); continue
        defining, _ = found[0]
        resolved.append(dict(owner=owner, kind=kind, name=name, descriptor=descriptor, defining=defining,
                             provider='fresh-application-dex' if defining in application else
                                      'selected-framework-dex' if defining in framework else 'genuine-core-compile-input'))
    result = dict(import_count=len(reproduced), imports=provenance, sdk_sha256=sha(args.android_jar),
                  selected_sdk_declarations=selected, selected_denominator=6, framework=framework_manifest,
                  application=application_manifest, application_compile_classes=classes_manifest,
                  native_source_sha256=sha(native_path), subscriber_native_methods=native_rows,
                  selected_closure=closure, framework_overlap=sorted(set(closure) & framework.keys()),
                  member_references=resolved, unresolved_members=unresolved, javap_command=command,
                  audit_sha256=sha(Path(__file__)), windows_execution=False,
                  limits=['Static six-declaration provider scope, not whole NSD/SDK/runtime coverage',
                          'Framework and application DEX are actual artifacts; core inherited declarations are compile inputs',
                          'Native loading, actual Windows sockets, watchers, Binder identity and lifecycle need runtime execution'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    if unresolved:
        raise ValueError('Unresolved selected members; inspect audit output')
    print(json.dumps(dict(imports=len(reproduced), sdk_declarations=len(selected), closure_classes=len(closure),
                          member_references=len(resolved), subscriber_native_methods=len(native_rows), windows_execution=False)))


if __name__ == '__main__':
    main()
