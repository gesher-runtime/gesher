#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Build owned SDK36 permission-reference APKs and frozen command arrays; never run ADB."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/nsd/permissions'
SOURCE = FIXTURES / 'src/gesher/qa/nsdpermissions/ControlReceiver.java'
VARIANTS = {
    'nointernet': [],
    'internet': ['android.permission.INTERNET'],
    'networkstate': ['android.permission.INTERNET', 'android.permission.ACCESS_NETWORK_STATE'],
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def executable(directory, name):
    path = directory / name
    if not path.is_file():
        path = directory / (name + '.exe')
    if not path.is_file():
        raise FileNotFoundError('Missing build tool: ' + str(path))
    return path


def validate_badging(name, badging, permissions):
    """Check flags from the built APK, not just the source manifest."""
    package = 'gesher.qa.nsdpermissions.' + name
    if "package: name='" + package + "'" not in badging or "targetSdkVersion:'36'" not in badging:
        raise RuntimeError('Unexpected APK package/target: ' + name)
    if 'application-debuggable' in badging:
        raise RuntimeError('Permission-reference APK must not be debuggable: ' + name)
    for permission in ('android.permission.INTERNET', 'android.permission.ACCESS_NETWORK_STATE'):
        if ("uses-permission: name='" + permission + "'" in badging) != (permission in permissions):
            raise RuntimeError('Unexpected APK permission: ' + name + ' ' + permission)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--android-jar', type=Path, required=True,
                        help='Official Android API36 SDK android.jar')
    parser.add_argument('--build-tools', type=Path, required=True,
                        help='Android SDK build-tools directory containing aapt2, zipalign and lib/')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out, sdk, build = args.output.resolve(), args.android_jar.resolve(), args.build_tools.resolve()
    out.mkdir(parents=True, exist_ok=True)
    java_bin = args.java_home.resolve() / 'bin'
    java, javac, jar, keytool = [executable(java_bin, name) for name in ('java', 'javac', 'jar', 'keytool')]
    aapt, align = [executable(build, name) for name in ('aapt2', 'zipalign')]
    d8, signer = build / 'lib/d8.jar', build / 'lib/apksigner.jar'
    for required in (sdk, d8, signer, SOURCE):
        if not required.is_file():
            raise FileNotFoundError(required)
    commands = []

    def run(name, arguments):
        command = list(map(str, arguments))
        began = time.monotonic()
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=90)
        (out / (name + '.log')).write_text(result.stdout, encoding='utf-8')
        commands.append({'name': name, 'command': command, 'exit_code': result.returncode,
                         'seconds': round(time.monotonic() - began, 3)})
        (out / 'build-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        if result.returncode:
            raise RuntimeError(name + ' failed; see ' + str(out / (name + '.log')))
        return result.stdout

    # This generated key is only for the three owned permission fixtures. It is neither
    # an application signing credential nor a checked-in private key.
    key, password = out / 'fixture.p12', 'owned-nsd-fixture-only'
    if not key.exists():
        run('fixture-key', [keytool, '-genkeypair', '-alias', 'nsd-reference', '-keyalg', 'RSA',
                           '-keysize', '2048', '-validity', '30', '-storetype', 'PKCS12',
                           '-keystore', key, '-storepass', password, '-keypass', password,
                           '-dname', 'CN=Owned Gesher NSD permission reference'])
    apks = []
    with tempfile.TemporaryDirectory(prefix='nsd-sdk-build-', dir=out) as temporary:
        work = Path(temporary)
        classes, dex = work / 'classes', work / 'dex'
        classes.mkdir(); dex.mkdir()
        run('javac', [javac, '-J-XX:-UsePerfData', '--release', '11', '-encoding', 'UTF-8',
                      '-cp', sdk, '-d', classes, SOURCE])
        fixture_jar = out / 'fixture.jar'
        run('jar', [jar, '--create', '--file', fixture_jar, '-C', classes, '.'])
        run('d8', [java, '-XX:-UsePerfData', '-cp', d8, 'com.android.tools.r8.D8',
                   '--min-api', '36', '--lib', sdk, '--output', dex, fixture_jar])
        shutil.copyfile(dex / 'classes.dex', out / 'classes.dex')
        for name, permissions in VARIANTS.items():
            variant = work / name
            variant.mkdir()
            package = 'gesher.qa.nsdpermissions.' + name
            manifest = FIXTURES / name / 'AndroidManifest.xml'
            unsigned, aligned, apk = variant / 'unsigned.apk', variant / 'aligned.apk', out / (name + '.apk')
            run(name + '-manifest', [aapt, 'link', '-I', sdk, '--manifest', manifest,
                                    '--version-code', '1', '--version-name', '1', '-o', unsigned])
            with zipfile.ZipFile(unsigned, 'a', compression=zipfile.ZIP_DEFLATED) as archive:
                archive.write(dex / 'classes.dex', 'classes.dex')
            run(name + '-align', [align, '-p', '-f', '4', unsigned, aligned])
            run(name + '-sign', [java, '-XX:-UsePerfData', '-jar', signer, 'sign', '--ks', key,
                                '--ks-pass', 'pass:' + password, '--key-pass', 'pass:' + password,
                                '--out', apk, aligned])
            run(name + '-verify', [java, '-XX:-UsePerfData', '-jar', signer,
                                  'verify', '--verbose', '--print-certs', apk])
            badging = run(name + '-badging', [aapt, 'dump', 'badging', apk])
            validate_badging(name, badging, permissions)
            apks.append({'name': name, 'package': package, 'permissions': permissions, 'debuggable': False,
                         'apk': str(apk), 'sha256': sha(apk), 'manifest': str(manifest),
                         'manifest_sha256': sha(manifest),
                         'badging_sha256': sha(out / (name + '-badging.log')),
                         'device_apk': '/data/local/tmp/gesher-nsd-perm-' + name + '-' + sha(apk)[:12] + '.apk'})

    receipt = {'identity': 'ordinary owned SDK APK process; target36; no system-context callsites',
               'scope': 'measure acquisition and ordinary/NetworkRequest permission outcomes',
               'source': str(SOURCE), 'source_sha256': sha(SOURCE),
               'android_jar': str(sdk), 'android_jar_sha256': sha(sdk),
               'dex_sha256': sha(out / 'classes.dex'), 'jar_sha256': sha(out / 'fixture.jar'),
               'build_script_sha256': sha(__file__),
               'tool_sha256': {str(path): sha(path) for path in (javac, aapt, align, d8, signer)},
               'apks': apks, 'executed': False}
    (out / 'manifest.json').write_text(json.dumps(receipt, indent=2) + '\n')
    setup = {'description': 'Push owned APKs read-only and verify hashes before installation',
             'manifest_sha256': sha(out / 'manifest.json'), 'steps': []}
    execution = {'description': 'Measure SDK/target36 NSD permissions under three ordinary APK identities',
                 'manifest_sha256': sha(out / 'manifest.json'), 'steps': [], 'expected_cases': []}
    cleanup = {'description': 'Remove only the three owned APKs and their uploaded files', 'steps': []}
    for apk in apks:
        setup['steps'].extend([
            {'adb': ['push', apk['apk'], apk['device_apk']], 'timeout': 90},
            {'adb': ['shell', 'chmod', '0444', apk['device_apk']], 'timeout': 30},
            {'adb': ['shell', 'sha256sum', apk['device_apk']], 'timeout': 30},
        ])
        execution['steps'].append({'label': 'install-' + apk['name'],
            'adb': ['shell', 'pm', 'install', '-r', apk['device_apk']], 'timeout': 90})
    for apk in apks:
        for mode in ('acquire', 'ordinary', 'request'):
            service_type = '_gq' + uuid.uuid4().hex[:10] + '._tcp'
            case = {'label': apk['name'] + '-' + mode, 'package': apk['package'],
                    'mode': mode, 'type': service_type, 'permissions': apk['permissions']}
            execution['expected_cases'].append(case)
            execution['steps'].append({'label': case['label'], 'adb': ['shell', 'am', 'broadcast',
                '--include-stopped-packages', '--receiver-foreground', '-n',
                apk['package'] + '/gesher.qa.nsdpermissions.ControlReceiver', '--es', 'mode', mode,
                '--es', 'type', service_type], 'timeout': 30})
            execution['steps'].append({'label': 'retire-' + case['label'],
                'adb': ['shell', 'am', 'force-stop', apk['package']], 'timeout': 30})
        cleanup['steps'].extend([
            {'adb': ['uninstall', apk['package']], 'timeout': 90},
            {'adb': ['shell', 'rm', apk['device_apk']], 'timeout': 30},
        ])
    for name, value in [('setup-request.json', setup), ('execute-request.json', execution),
                        ('cleanup-request.json', cleanup)]:
        (out / name).write_text(json.dumps(value, indent=2) + '\n')
    print(json.dumps({'source_sha256': receipt['source_sha256'], 'dex_sha256': receipt['dex_sha256'],
                      'apks': [{key: apk[key] for key in ('name', 'package', 'sha256')} for apk in apks],
                      'executed': False}, indent=2))


if __name__ == '__main__':
    main()
