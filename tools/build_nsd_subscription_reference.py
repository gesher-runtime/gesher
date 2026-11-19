#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Freeze an ordinary SDK36 subscription reference APK; write, never execute, device requests."""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import shutil
import subprocess
import time
import zipfile

from build_nsd_permission_reference import executable

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tests/nsd/subscription-reference'
PACKAGE = 'gesher.qa.nsdsubscription'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'android-jar', 'build-tools', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out, sdk, build = args.output.resolve(), args.android_jar.resolve(), args.build_tools.resolve()
    out.mkdir(parents=True, exist_ok=False)
    copied = out / 'source'; shutil.copytree(SOURCE, copied)
    java_bin = args.java_home.resolve() / 'bin'
    java, javac, jar, keytool = [executable(java_bin, name) for name in ('java', 'javac', 'jar', 'keytool')]
    aapt, align = [executable(build, name) for name in ('aapt2', 'zipalign')]
    d8, signer = build / 'lib/d8.jar', build / 'lib/apksigner.jar'
    commands = []

    def run(label, arguments):
        command = list(map(str, arguments)); begin = time.monotonic()
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
        (out / (label + '.log')).write_text(result.stdout)
        commands.append(dict(label=label, command=command, exit=result.returncode, seconds=time.monotonic() - begin))
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        if result.returncode:
            raise RuntimeError(label + ' failed: ' + result.stdout)
        return result.stdout

    classes, dex = out / 'classes', out / 'dex'; classes.mkdir(); dex.mkdir()
    run('javac', [javac, '-J-XX:-UsePerfData', '--release', '17', '-encoding', 'UTF-8', '-cp', sdk,
                  '-d', classes, copied / 'ReferenceActivity.java'])
    compiled = out / 'fixture.jar'
    run('jar', [jar, '--create', '--file', compiled, '-C', classes, '.'])
    run('d8', [java, '-XX:-UsePerfData', '-cp', d8, 'com.android.tools.r8.D8', '--min-api', '36',
               '--lib', sdk, '--output', dex, compiled])
    unsigned, aligned, apk = out / 'unsigned.apk', out / 'aligned.apk', out / 'subscription-reference.apk'
    run('manifest', [aapt, 'link', '-I', sdk, '--manifest', copied / 'AndroidManifest.xml',
                     '--version-code', '1', '--version-name', '1', '-o', unsigned])
    with zipfile.ZipFile(unsigned, 'a', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(dex / 'classes.dex', 'classes.dex')
    run('align', [align, '-p', '-f', '4', unsigned, aligned])
    key, password = out / 'fixture.p12', 'owned-subscription-fixture-only'
    run('fixture-key', [keytool, '-genkeypair', '-alias', 'owned-reference', '-keyalg', 'RSA', '-keysize', '2048',
                        '-validity', '30', '-storetype', 'PKCS12', '-keystore', key, '-storepass', password,
                        '-keypass', password, '-dname', 'CN=Owned Gesher NSD subscription reference'])
    run('sign', [java, '-XX:-UsePerfData', '-jar', signer, 'sign', '--ks', key, '--ks-pass', 'pass:' + password,
                 '--key-pass', 'pass:' + password, '--out', apk, aligned])
    run('verify', [java, '-XX:-UsePerfData', '-jar', signer, 'verify', '--verbose', '--print-certs', apk])
    badging = run('badging', [aapt, 'dump', 'badging', apk])
    if "package: name='" + PACKAGE + "'" not in badging or "targetSdkVersion:'36'" not in badging:
        raise RuntimeError('Unexpected APK identity or target')
    permissions = ['android.permission.INTERNET', 'android.permission.ACCESS_NETWORK_STATE']
    if sorted(line for line in badging.splitlines() if line.startswith('uses-permission:')) != sorted(
            "uses-permission: name='" + value + "'" for value in permissions):
        raise RuntimeError('Unexpected APK permissions')
    device_apk = '/data/local/tmp/gesher-nsd-subscription-' + sha(apk)[:12] + '.apk'
    manifest = dict(package=PACKAGE, targetSdk=36, permissions=permissions, apk=str(apk), apk_sha256=sha(apk),
                    device_apk=device_apk, java_source_sha256=sha(copied / 'ReferenceActivity.java'),
                    manifest_sha256=sha(copied / 'AndroidManifest.xml'), android_jar_sha256=sha(sdk),
                    dex_sha256=sha(dex / 'classes.dex'), jar_sha256=sha(compiled),
                    class_hashes={str(p.relative_to(classes)): sha(p) for p in classes.rglob('*.class')},
                    tool_hashes={str(p): sha(p) for p in (javac, aapt, align, d8, signer)},
                    builder_sha256=sha(__file__), builder_helper_sha256=sha(ROOT / 'tools/build_nsd_permission_reference.py'),
                    executed=False, identity='ordinary APK, public SDK36 callsites, owned valid multicast peer')
    manifest_path = out / 'manifest.json'; manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    service_type = '_gr' + secrets.token_hex(6) + '._tcp'
    setup = dict(description='Push owned immutable NSD subscription APK and verify hash',
                 manifest_sha256=sha(manifest_path), steps=[
                     dict(adb=['push', str(apk), device_apk], timeout=30),
                     dict(adb=['shell', 'chmod', '0444', device_apk], timeout=30),
                     dict(adb=['shell', 'sha256sum', device_apk], timeout=30)])
    execution = dict(description='Install and start ordinary SDK36 callback reference; inspect only its own result',
                     manifest_sha256=sha(manifest_path), service_type=service_type, steps=[
                         dict(adb=['shell', 'pm', 'install', '-r', device_apk], timeout=60),
                         dict(adb=['shell', 'run-as', PACKAGE, 'rm', '-f', 'files/result.json'], timeout=30),
                         dict(adb=['shell', 'am', 'start', '-W', '-n', PACKAGE + '/.ReferenceActivity',
                                   '--es', 'type', service_type], timeout=30)],
                     result_request=dict(adb=['shell', 'run-as', PACKAGE, 'cat', 'files/result.json'], timeout=30),
                     observation_deadline_seconds=60,
                     interpretation='Result must record passed=true, actual SDK/target36 and matching ordinary process/Binder UID. Missing file is pending, not success.')
    cleanup = dict(description='Retire only this owned fixture and remove its uploaded APK', steps=[
                       dict(adb=['shell', 'am', 'force-stop', PACKAGE], timeout=30),
                       dict(adb=['uninstall', PACKAGE], timeout=30),
                       dict(adb=['shell', 'rm', device_apk], timeout=30)])
    for name, value in (('setup-request.json', setup), ('execute-request.json', execution), ('cleanup-request.json', cleanup)):
        (out / name).write_text(json.dumps(value, indent=2) + '\n')
    print(json.dumps({'manifest': str(manifest_path), 'manifest_sha256': sha(manifest_path),
                      'apk_sha256': sha(apk), 'executed': False}, indent=2))


if __name__ == '__main__':
    main()
