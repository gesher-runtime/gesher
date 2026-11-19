#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Build a frozen public SDK36 owned-service reference without invoking ADB."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def archive(path, files):
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as output:
        for name, data in files:
            item = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            output.writestr(item, data)


def run(command, log):
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f'{log.name}: {result.stdout}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'java-home', 'android-jar', 'd8-jar', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve()
    java_home = args.java_home.resolve()
    sdk = args.android_jar.resolve()
    d8 = args.d8_jar.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    inputs = output / 'input-sources'
    inputs.mkdir()
    frozen = inputs / source.name
    shutil.copyfile(source, frozen)
    shutil.copyfile(Path(__file__), inputs / Path(__file__).name)
    classes = output / 'classes'
    classes.mkdir()
    dex = output / 'dex'
    dex.mkdir()
    commands = [[str(java_home / 'bin/javac'), '-source', '17', '-target', '17',
                 '-encoding', 'UTF-8', '-XDstringConcat=inline', '-cp', str(sdk),
                 '-d', str(classes), str(frozen)]]
    run(commands[0], output / 'javac.log')
    class_files = sorted(classes.rglob('*.class'))
    if not class_files or any(not value.relative_to(classes).as_posix().startswith('gesher/nsd/')
                              for value in class_files):
        raise RuntimeError('Reference must define gesher.nsd classes only')
    class_jar = output / 'reference-classes.jar'
    archive(class_jar, [(value.relative_to(classes).as_posix(), value.read_bytes())
                        for value in class_files])
    commands.append([str(java_home / 'bin/java'), '-Xmx1g', '-cp', str(d8),
                     'com.android.tools.r8.D8', '--min-api', '36', '--lib', str(sdk),
                     '--output', str(dex), str(class_jar)])
    run(commands[-1], output / 'd8.log')
    dex_files = sorted(dex.glob('*.dex'))
    if not dex_files:
        raise RuntimeError('D8 produced no DEX payload')
    payload = output / 'reference.jar'
    archive(payload, [(value.name, value.read_bytes()) for value in dex_files])
    remote = '/data/local/tmp/gesher-nsd-registration-reference-' + digest(frozen)[:12]
    remote_jar = remote + '/reference.jar'
    setup = {'description': 'Install immutable public SDK36 owned registration reference', 'steps': [
        {'adb': ['shell', 'mkdir', '-p', remote], 'timeout': 30},
        {'adb': ['push', str(payload), remote_jar], 'timeout': 90},
        {'adb': ['shell', 'chmod', '0444', remote_jar], 'timeout': 30},
        {'adb': ['shell', 'sha256sum', remote_jar], 'timeout': 30}]}
    execute = {'description': 'Stock API36 registration with owned valid DNS peers and TCP endpoints', 'steps': [
        {'adb': ['shell', 'env', 'CLASSPATH=' + remote_jar, 'app_process', '/system/bin',
                 'gesher.nsd.NsdRegistrationAndroidReference'], 'timeout': 180}]}
    cleanup = {'description': 'Remove only the owned registration reference payload and directory', 'steps': [
        {'adb': ['shell', 'rm', '-f', remote_jar], 'timeout': 30},
        {'adb': ['shell', 'rmdir', remote], 'timeout': 30}]}
    requests = {'setup': setup, 'run': execute, 'cleanup': cleanup}
    for name, request in requests.items():
        (output / (name + '-request.json')).write_text(json.dumps(request, indent=2) + '\n')
    version = subprocess.run([str(java_home / 'bin/javac'), '-version'],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, check=True).stdout.strip()
    manifest = {
        'scope': 'Stock Android API36 ordinary registration/unregister overloads; actual owned IPv4 DNS and TCP peers. app_process system context, not ordinary APK permission qualification. No Windows execution.',
        'source': {'path': str(source), 'snapshot': str(frozen), 'sha256': digest(frozen)},
        'builderSha256': digest(Path(__file__)),
        'javac': {'path': str(java_home / 'bin/javac'), 'version': version,
                  'sha256': digest(java_home / 'bin/javac')},
        'compilationSdk': {'path': str(sdk), 'sha256': digest(sdk)},
        'd8': {'path': str(d8), 'sha256': digest(d8)},
        'classesJarSha256': digest(class_jar),
        'payload': {'path': str(payload), 'sha256': digest(payload), 'devicePath': remote_jar},
        'classes': {value.relative_to(classes).as_posix(): digest(value) for value in class_files},
        'dex': {value.name: digest(value) for value in dex_files},
        'providerBoundary': 'Only gesher.nsd is defined. Runtime assertions require NsdManager and NsdServiceInfo to share their actual platform provider, separate from the capsule. No platform replacements or flag overrides.',
        'txtBoundary': 'Public setAttribute(String,String) only, including NUL and UTF-8, null and empty values. No hidden byte-array setter and no arbitrary non-UTF8 claim.',
        'peerProfile': 'multicast-5353-fresh-owned-wire',
        'peerBoundary': 'Normal standard QM multicast questions from port5353 for UUID-owned service names and their actually observed SRV targets. Fresh packets during each query window are required. Periodic announcements can supply records; causal query response and legacy-unicast behavior are not qualified. TCP uses decoded actual owned address and listening port.',
        'commands': commands,
        'requestSha256': {name: digest(output / (name + '-request.json')) for name in requests},
        'device_executed': False,
        'pre_execution_review': 'pending',
    }
    manifest_path = output / 'manifest.json'
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'manifest': str(manifest_path), 'manifestSha256': digest(manifest_path),
                      'payloadSha256': digest(payload), 'sourceSha256': digest(frozen),
                      'classes': len(class_files)}))


if __name__ == '__main__':
    main()
