#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Audit the selected registration SDK surface, pinned imports and defining inputs.

The SDK supplies declarations only. Actual selected framework DEX, fresh complete
application classes and genuine core compile classes remain distinct providers.
This static evidence does not qualify Windows loading, Binder or network delivery.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

import java_member_inventory as members
import nsd_registration_imports

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def classes(folder):
    result, paths = {}, {}
    for path in folder.rglob('*.class'):
        if path.name == 'module-info.class':
            continue
        item = members.class_members(path.read_bytes())
        assert item.name not in result, 'Duplicate class definition: ' + item.name
        result[item.name], paths[item.name] = item, path
    return result, paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'android-jar', 'framework-dex', 'application-classes',
                 'core-classes', 'upstream-sources', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    provenance = json.loads((ROOT / 'docs/research/nsd-registration-core-sources.json').read_text())
    for name, item in provenance['inputs'].items():
        assert digest(args.upstream_sources / name) == item['sha256'], name + ' raw source changed'
    generated = nsd_registration_imports.sources(args.upstream_sources)
    for name, text in generated.items():
        path = ROOT / 'app/java/com/gesher/net/mdns' / name
        assert path.read_bytes() == text.encode(), name + ' undeclared source adaptation'
        assert digest(path) == provenance['outputs'][name]['sha256'], name + ' output hash changed'
    assert len(generated) == 5

    application, application_paths = classes(args.application_classes)
    core, core_paths = classes(args.core_classes)
    framework, framework_manifest = members.archive_members(args.framework_dex, 'boot', 'selected-framework')
    selected, sdk_types = [], {}
    with zipfile.ZipFile(args.android_jar) as sdk:
        for name in ('android.net.nsd.NsdManager', 'android.net.nsd.NsdManager$RegistrationListener',
                     'android.net.nsd.AdvertisingRequest'):
            sdk_types[name] = members.class_members(sdk.read(name.replace('.', '/') + '.class'))
        for name in ('android.net.nsd.NsdManager', 'android.net.nsd.NsdManager$RegistrationListener'):
            for item in sdk_types[name].members.values():
                if item.kind != 'method' or not item.flags & members.PUBLIC or item.name.startswith('<'):
                    continue
                if name.endswith('NsdManager') and item.name not in ('registerService', 'unregisterService'):
                    continue
                actual = application[name].members.get(item.key)
                assert actual is not None and not members.flag_issues(item, actual), (name, item.key)
                selected.append({'owner': name, 'name': item.name, 'descriptor': item.descriptor,
                                 'sdk_flags': item.flags, 'candidate_flags': actual.flags})
    assert len(selected) == 7, 'Selected SDK36 registration denominator changed'
    hidden = ('method', 'registerService', '(Landroid/net/nsd/AdvertisingRequest;Ljava/util/concurrent/Executor;Landroid/net/nsd/NsdManager$RegistrationListener;)V')
    assert hidden not in sdk_types['android.net.nsd.NsdManager'].members
    assert hidden in application['android.net.nsd.NsdManager'].members

    javap = str(args.java_home / 'bin/javap')
    constants = {}
    constant_commands = []
    for name in ('android.net.nsd.NsdManager', 'android.net.nsd.AdvertisingRequest'):
        values = []
        for classpath in (args.android_jar, args.application_classes):
            command = [javap, '-public', '-constants', '-classpath', str(classpath), name]
            constant_commands.append(command)
            text = subprocess.check_output(command, text=True)
            values.append(dict(re.findall(r'public static final (?:int|long) (\w+) = ([^;]+);', text)))
        names = [n for n in values[0] if n.startswith('FAILURE_') or n in ('PROTOCOL_DNS_SD', 'FLAG_SKIP_PROBING')]
        for constant in names:
            assert values[0][constant] == values[1].get(constant), (name, constant)
            constants[name + '.' + constant] = values[0][constant]

    prefixes = ('com.gesher.net.mdns.', 'com.gesher.net.WindowsNsdAdvertiser',
                'com.gesher.net.NsdRegistration', 'com.gesher.net.INsdRegistration',
                'com.gesher.server.NsdRegistrationService', 'android.net.nsd.NsdManager',
                'android.net.nsd.AdvertisingRequest', 'android.net.nsd.NsdServiceInfo')
    closure = sorted(name for name in application if name.startswith(prefixes))
    command = [javap, '-verbose', '-classpath', str(args.application_classes), *closure]
    text = subprocess.check_output(command, text=True)
    references = set()
    for kind, owner, name, descriptor in re.findall(
            r'\s*#\d+ = (Fieldref|Methodref|InterfaceMethodref).*?// ([\w/$]+)\.([^:]+):([^\r\n]+)', text):
        owner = owner.replace('/', '.')
        if owner.startswith(('android.', 'com.android.', 'com.gesher.', 'java.', 'javax.')):
            references.add((owner, 'field' if kind == 'Fieldref' else 'method', name.strip('"'), descriptor.strip()))
    available = {**core, **framework, **application}
    resolver = members.Resolver(available)
    resolved, unresolved = [], []
    for owner, kind, name, descriptor in sorted(references):
        if owner not in available:
            unresolved.append([owner, kind, name, descriptor, 'owner absent'])
            continue
        found, uncertain = resolver.lookup(owner, (kind, name, descriptor))
        if len(found) != 1 or uncertain:
            unresolved.append([owner, kind, name, descriptor, 'missing/ambiguous/incomplete hierarchy'])
            continue
        defining, _ = found[0]
        provider = ('fresh-application-class' if defining in application else
                    'selected-framework-dex' if defining in framework else 'genuine-core-compile-class')
        resolved.append({'owner': owner, 'kind': kind, 'name': name, 'descriptor': descriptor,
                         'defining_class': defining, 'provider': provider})

    native = ROOT / 'compat/net/windows_nsd_resolve.cc'
    table = re.search(r'const JNINativeMethod kAdvertiserMethods\[\] = \{(.*?)\n\};', native.read_text(), re.S)
    assert table is not None
    native_rows = re.findall(r'\{"(native\w+)", "([^"]+)", NATIVEHELPER_SYSV_JNI_ENTRY\((\w+)\)\}', table.group(1))
    declared = {item.name: item.descriptor for item in application['com.gesher.net.WindowsNsdAdvertiser'].members.values()
                if item.flags & members.NATIVE and item.flags & members.STATIC}
    assert len(native_rows) == len(declared) == 9
    assert {n: d for n, d, _ in native_rows} == declared
    output = {
        'pinned_imports': provenance, 'reproduced_import_count': len(generated),
        'sdk_sha256': digest(args.android_jar), 'selected_sdk_declarations': selected,
        'additional_hidden_manager_descriptor': hidden[2], 'selected_sdk_constants': constants,
        'framework': framework_manifest,
        'actual_framework_overlap_with_application_closure': sorted(set(closure) & framework.keys()),
        'application_closure_hashes': {n: digest(application_paths[n]) for n in closure},
        'member_references': resolved, 'unresolved_members': unresolved,
        'core_compile_provider_hashes': {r['defining_class']: digest(core_paths[r['defining_class']])
                                       for r in resolved if r['provider'] == 'genuine-core-compile-class'},
        'native_source_sha256': digest(native), 'native_methods': native_rows,
        'javap_command': command, 'constant_commands': constant_commands,
        'tool_sha256': digest(Path(__file__)),
        'limits': ['Seven selected SDK36 declarations, not whole NSD/runtime coverage.',
                   'Additional hidden manager/value paths are named source profiles, not SDK36 public declarations.',
                   'Actual selected framework DEX plus fresh application classes; not a complete shipped boot/core DEX census.',
                   'Java/core inherited declarations are genuine compile inputs, not packaged Windows provider execution.',
                   'No native loading, remote Binder, socket, permissions or lifecycle qualification follows from this static audit.'],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    assert not unresolved, 'Selected registration closure has unresolved members; inspect output'
    print(json.dumps({'imports': len(generated), 'sdk_declarations': len(selected),
                      'constants': len(constants), 'closure_classes': len(closure),
                      'member_references': len(resolved), 'native_methods': len(native_rows),
                      'windowsExecution': False}))


if __name__ == '__main__':
    main()
