#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Audit pinned resolver imports, selected SDK descriptors and actual provider inputs.

SDK stubs specify the seven selected declarations; they never supply runtime
providers. This static audit does not execute the Windows native transport.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

import java_member_inventory as members

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'android-jar', 'framework-dex', 'application-classes', 'core-classes',
                 'upstream-sources', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    provenance = ROOT / 'docs/research/nsd-resolver-codec-sources.json'
    imported = json.loads(provenance.read_text())
    verified = {}
    for name, expected in imported['files'].items():
        raw = args.upstream_sources / name
        adapted = ROOT / 'app/java/com/gesher/net/mdns' / name
        assert digest(raw) == expected['upstream_sha256'], name + ' upstream hash'
        text = raw.read_text().replace('com.android.server.connectivity.mdns', 'com.gesher.net.mdns')
        for annotation in ('VisibleForTesting', 'ChecksSdkIntAtLeast'):
            text = text.replace('import androidx.annotation.' + annotation + ';\n', '')
            text = re.sub(r'(?m)^[ \t]*@' + annotation + r'\([^\n]*\)', '', text)
        assert adapted.read_bytes() == text.encode(), name + ' undeclared source adaptation'
        assert digest(adapted) == expected['adapted_sha256'], name + ' adapted hash'
        verified[name] = expected
    assert len(verified) == 15
    application = {}
    application_paths = {}
    for path in args.application_classes.rglob('*.class'):
        if path.name == 'module-info.class':
            continue
        parsed = members.class_members(path.read_bytes())
        assert parsed.name not in application, 'duplicate application provider'
        application[parsed.name] = parsed
        application_paths[parsed.name] = path

    selected = []
    with zipfile.ZipFile(args.android_jar) as sdk:
        for name in ('android.net.nsd.NsdManager', 'android.net.nsd.NsdManager$ResolveListener'):
            specified = members.class_members(sdk.read(name.replace('.', '/') + '.class'))
            for item in specified.members.values():
                if item.kind != 'method' or not item.flags & members.PUBLIC or item.name.startswith('<'):
                    continue
                if name.endswith('NsdManager') and item.name not in ('resolveService', 'stopServiceResolution'):
                    continue
                actual = application[name].members.get(item.key)
                assert actual is not None, (name, item.key)
                assert not members.flag_issues(item, actual), (name, item.key, 'access/static mismatch')
                selected.append({'owner': name, 'name': item.name, 'descriptor': item.descriptor,
                                 'sdk_flags': item.flags, 'candidate_flags': actual.flags})
    assert len(selected) == 7, 'selected SDK denominator changed'

    native = ROOT / 'compat/net/windows_nsd_resolve.cc'
    native_text = native.read_text()

    def native_table(symbol):
        table = re.search(r'const JNINativeMethod ' + re.escape(symbol)
                          + r'\[\] = \{(.*?)\n\};', native_text, re.S)
        assert table is not None, symbol + ' table missing'
        return re.findall(r'\{"(native\w+)", "([^"]+)", NATIVEHELPER_SYSV_JNI_ENTRY\((\w+)\)\}',
                          table.group(1))

    # These are distinct providers even though they share one real native
    # library/registry. Never count both tables as seventeen resolver methods.
    native_rows = native_table('kMethods')
    declared = {item.name: item.descriptor for item in
                application['com.gesher.net.WindowsNsdResolver'].members.values()
                if item.flags & members.NATIVE and item.flags & members.STATIC
                and not item.name.startswith('nativeSubscription')}
    assert len(native_rows) == len(declared) == 8
    assert {name: descriptor for name, descriptor, _ in native_rows} == declared
    # Continuous observation has its own JNI/provider qualification. Keep its
    # declarations separate from the unchanged one-shot eight-method denominator.
    subscription_rows = native_table('kSubscriptionMethods')
    subscription_declared = {item.name: item.descriptor for item in
                            application['com.gesher.net.WindowsNsdResolver'].members.values()
                            if item.flags & members.NATIVE and item.flags & members.STATIC
                            and item.name.startswith('nativeSubscription')}
    assert len(subscription_rows) == len(subscription_declared) == 8
    assert {name: descriptor for name, descriptor, _ in subscription_rows} == subscription_declared
    assert ('method', '<init>', '(JIII[B)V') in application['com.gesher.net.WindowsNsdResolver$Packet'].members
    assert '"(JIII[B)V"' in native.read_text(), 'native Packet constructor descriptor'
    advertiser_rows = native_table('kAdvertiserMethods')
    advertiser_declared = {item.name: item.descriptor for item in
                          application['com.gesher.net.WindowsNsdAdvertiser'].members.values()
                          if item.flags & members.NATIVE and item.flags & members.STATIC}
    assert len(advertiser_rows) == len(advertiser_declared) == 9
    assert {name: descriptor for name, descriptor, _ in advertiser_rows} == advertiser_declared
    advertiser_constructors = {'Packet': '(JIII[BIZI[B)V', 'Address': '(JII[BIIIIII)V'}
    for name, descriptor in advertiser_constructors.items():
        assert ('method', '<init>', descriptor) in application[
            'com.gesher.net.WindowsNsdAdvertiser$' + name].members
        assert '"' + descriptor + '"' in native_text, 'native advertiser ' + name + ' constructor'

    framework, framework_manifest = members.archive_members(args.framework_dex, 'boot', 'selected-framework')
    # Genuine core classfiles close inherited declarations such as
    # HandlerThread.start -> Thread.start. These are compile inputs, not proof
    # that a particular packaged core DEX defines the methods at runtime.
    core, core_paths = {}, {}
    for path in args.core_classes.rglob('*.class'):
        if path.name == 'module-info.class':
            continue
        parsed = members.class_members(path.read_bytes())
        core[parsed.name] = parsed
        core_paths[parsed.name] = path
    available = {**core, **framework, **application}
    resolver = members.Resolver(available)
    closure = sorted(name for name in application if name.startswith((
        'com.gesher.net.mdns.', 'com.gesher.net.WindowsNsdResolver', 'android.net.nsd.NsdManager')))
    command = [str(args.java_home / 'bin/javap'), '-verbose', '-classpath', str(args.application_classes), *closure]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    references = set()
    for kind, owner, name, descriptor in re.findall(
            r'\s*#\d+ = (Fieldref|Methodref|InterfaceMethodref).*?// ([\w/$]+)\.([^:]+):([^\r\n]+)', result.stdout):
        owner = owner.replace('/', '.')
        if owner.startswith(('android.', 'com.android.', 'com.gesher.')):
            references.add((owner, 'field' if kind == 'Fieldref' else 'method', name.strip('"'), descriptor.strip()))
    resolved = []
    unresolved = []
    for owner, kind, name, descriptor in sorted(references):
        if owner not in available:
            unresolved.append([owner, kind, name, descriptor, 'owner absent'])
            continue
        found, uncertain = resolver.lookup(owner, (kind, name, descriptor))
        if len(found) != 1 or uncertain:
            unresolved.append([owner, kind, name, descriptor, 'missing/ambiguous/incomplete hierarchy'])
            continue
        defining, _ = found[0]
        provider = ('fresh-application-classes' if defining in application else
                    'selected-framework-dex' if defining in framework else 'genuine-core-compile-class')
        resolved.append({'owner': owner, 'kind': kind, 'name': name, 'descriptor': descriptor,
                         'defining_class': defining, 'provider': provider})
    output = {
        'codec_imports': verified, 'sdk_sha256': digest(args.android_jar),
        'sdk_selected_declarations': selected, 'native_source_sha256': digest(native),
        'native_signatures': [{'name': n, 'descriptor': d, 'function': f} for n, d, f in native_rows],
        'separate_subscription_native_signatures': [{'name': n, 'descriptor': d, 'function': f}
                                                   for n, d, f in subscription_rows],
        'advertiser_native_signatures': [{'name': n, 'descriptor': d, 'function': f}
                                         for n, d, f in advertiser_rows],
        'advertiser_constructor_descriptors': advertiser_constructors,
        'advertiser_jni_class_hashes': {name: digest(application_paths[name]) for name in (
            'com.gesher.net.WindowsNsdAdvertiser', 'com.gesher.net.WindowsNsdAdvertiser$Packet',
            'com.gesher.net.WindowsNsdAdvertiser$Address')},
        'packet_constructor_descriptor': '(JIII[B)V', 'framework': framework_manifest,
        'application_closure_hashes': {name: digest(application_paths[name]) for name in closure},
        'called_android_gesher_members': resolved, 'unresolved_members': unresolved,
        'inherited_core_compile_hashes': {row['defining_class']: digest(core_paths[row['defining_class']])
                for row in resolved if row['provider'] == 'genuine-core-compile-class'},
        'javap_command': command, 'tool_sha256': digest(Path(__file__)),
        'limits': ['Selected 7 SDK36 declarations, not whole NSD/runtime coverage',
                   'Advertiser extension checks 9 JNI methods and 2 constructors only; its full Java/core/SDK behavior has a separate registration audit',
                   'Actual selected framework DEX plus fresh application class inputs; not a complete boot DEX census',
                   'java.*, javax.* and other core-provider members are outside this selected member audit',
                   'Inherited core declarations resolved from genuine classfiles are compile evidence, not packaged core DEX provider proof',
                   'Static descriptor/import checks do not qualify native registration, multicast delivery or retirement on Windows'],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    assert not unresolved, 'Selected member linkage unresolved; see output'
    print(json.dumps({'imports': len(verified), 'sdk_declarations': len(selected),
                      'native_signatures': len(native_rows), 'advertiser_native_signatures': len(advertiser_rows),
                      'closure_classes': len(closure),
                      'called_members': len(resolved), 'windowsExecution': False}))


if __name__ == '__main__':
    main()
