#!/usr/bin/env python3
"""SDK-compiled Wi-Fi data contracts over the production Android 16 implementations.

The host uses the real framework classes and the existing native initialization
adapter. It omits native Parcel transport; the same fixture on Windows requires
real android_runtime and Parcelable round trips. Negative controls replace only
the selected old class after current production compilation, retaining the other
value types to isolate the missing public descriptor.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

import java_member_inventory as members

ROOT = Path(__file__).resolve().parents[1]


def check_scan_declarations(sdk_jar: Path, classes: Path) -> dict:
    """Reuse the member inventory parser for these concrete, freshly built value types.

    This checks declarations and access flags only, not a boot-class-path percentage
    or radio support. Every selected upstream value type declares its SDK members.
    """
    result = {}
    with zipfile.ZipFile(sdk_jar) as sdk:
        for name in ('ScanResult', 'ScanResult$InformationElement', 'MloLink'):
            relative = 'android/net/wifi/' + name + '.class'
            expected = members.class_members(sdk.read(relative))
            actual_bytes = (classes / relative).read_bytes()
            actual = members.class_members(actual_bytes)
            count = 0
            for member in expected.members.values():
                if not member.flags & (members.PUBLIC | members.PROTECTED):
                    continue
                count += 1
                provided = actual.members.get(member.key)
                if provided is None:
                    raise AssertionError(f'{expected.name}: absent declaration {member.key}')
                issues = members.flag_issues(member, provided)
                if issues:
                    raise AssertionError(f'{expected.name}: {member.key}: {issues}')
            result[expected.name] = {'sdk_public_protected_declarations': count,
                                     'missing_or_incompatible_declarations': 0,
                                     'compiled_sha256': hashlib.sha256(actual_bytes).hexdigest()}
    return {'scope': 'selected compiled value-type declarations; not full runtime coverage',
            'sdk_sha256': hashlib.sha256(sdk_jar.read_bytes()).hexdigest(), 'classes': result}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--sdk-jar', type=Path, required=True)
    parser.add_argument('--framework-classes', type=Path, required=True)
    parser.add_argument('--core-classes', type=Path, required=True)
    parser.add_argument('--source-ref', help='Use an old WifiInfo source as a negative control')
    parser.add_argument('--scanresult-source-ref',
                        help='Use an old ScanResult source as a negative control')
    parser.add_argument('--inventory-output', type=Path,
                        help='Write the selected compiled-declaration audit (positive run only)')
    args = parser.parse_args()
    if args.inventory_output and (args.source_ref or args.scanresult_source_ref):
        parser.error('--inventory-output cannot describe an intentionally replaced negative-control class')
    cc = shutil.which('cc')
    if not cc:
        parser.error('A C compiler is required for the declared native initialization boundary')
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.framework_classes, args.core_classes))
    javac = str(args.java_home.resolve() / 'bin/javac')
    java = str(args.java_home.resolve() / 'bin/java')
    with tempfile.TemporaryDirectory(prefix='gesher-wifi-data-contracts-') as temporary:
        work = Path(temporary)
        classes, sdk_classes = work / 'classes', work / 'sdk-classes'
        classes.mkdir()
        sdk_classes.mkdir()
        library = work / 'libwifi_data_host.so'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home.resolve() / 'include'),
                        '-I' + str(args.java_home.resolve() / 'include/linux'),
                        str(ROOT / 'tests/component-caller/framework_host.c'), '-o', str(library)],
                       check=True)
        # Every test call site binds to the actual public Android SDK descriptors.
        subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', str(args.sdk_jar.resolve()), '-d', str(sdk_classes),
                        str(ROOT / 'tests/dex-services/gesher/services/WifiDataContractTest.java'),
                        str(ROOT / 'tests/dex-services/gesher/services/WifiScanResultContractTest.java')],
                       check=True)
        sources = []
        for name in ('wifi/WifiInfo', 'wifi/WifiSsid', 'wifi/SupplicantState',
                     'wifi/util/HexEncoding', 'wifi/ScanResult', 'wifi/MloLink',
                     'wifi/AnqpInformationElement', 'wifi/ScanResultSecurity',
                     'wifi/WifiConfiguration', 'wifi/WifiEnterpriseConfig',
                     'wifi/SecurityParams', 'wifi/ParcelUtil', 'wifi/OuiKeyedData',
                     'wifi/util/PersistableBundleUtils',
                     'NetworkInfo', 'TransportInfo', 'MacAddress'):
            relative = 'app/java/android/net/' + name + '.java'
            source = ROOT / relative
            sources.append(str(source))
        sources.append(str(ROOT / 'app/java/com/android/net/module/util/MacAddressUtils.java'))
        subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', classpath, '-d', str(classes), *sources], check=True)
        if not (args.source_ref or args.scanresult_source_ref):
            inventory = check_scan_declarations(args.sdk_jar, classes)
            if args.inventory_output:
                args.inventory_output.write_text(json.dumps(inventory, indent=2) + '\n', encoding='utf-8')
            print('Selected SDK declarations:', {name: data['sdk_public_protected_declarations']
                  for name, data in inventory['classes'].items()}, flush=True)
        for name, ref in (('WifiInfo', args.source_ref),
                          ('ScanResult', args.scanresult_source_ref)):
            if not ref:
                continue
            relative = 'app/java/android/net/wifi/' + name + '.java'
            old = subprocess.run(['git', 'show', ref + ':' + relative],
                                 cwd=ROOT, capture_output=True, check=True)
            source = work / (name + '.java')
            source.write_bytes(old.stdout)
            subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                            '-cp', os.pathsep.join((str(classes), classpath)),
                            '-d', str(classes), str(source)], check=True)
        subprocess.run([java, '-XX:-UsePerfData', '-XX:+UnlockDiagnosticVMOptions',
                        '-XX:-BytecodeVerificationRemote', '-cp',
                        os.pathsep.join((str(classes), str(sdk_classes), classpath)),
                        'gesher.services.WifiDataContractTest', str(library)],
                       check=True, timeout=60)
        print('Public SDK call sites and real production Wi-Fi value types; '
              'host initialization adapter only. Native Parcel is a separate Windows gate.')


if __name__ == '__main__':
    main()
