#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Build a frozen SDK36-only stock NSD binary-resolution reference; never invoke ADB."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def archive(path, files):
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as output:
        for name, data in files:
            item = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            output.writestr(item, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'android-jar', 'd8-jar', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'tests/nsd/NsdResolveAndroidReference.java'
    inputs = output / 'input-sources'; inputs.mkdir()
    frozen = inputs / source.name; shutil.copyfile(source, frozen)
    shutil.copyfile(Path(__file__), inputs / Path(__file__).name)
    classes = output / 'classes'; classes.mkdir()
    dex = output / 'dex'; dex.mkdir()
    commands = [[str(args.java_home / 'bin/javac'), '-source', '17', '-target', '17',
                 '-encoding', 'UTF-8', '-XDstringConcat=inline', '-cp', str(args.android_jar),
                 '-d', str(classes), str(frozen)]]
    result = subprocess.run(commands[0], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (output / 'javac.log').write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(result.stdout)
    class_files = sorted(classes.rglob('*.class'))
    if any(not str(value.relative_to(classes)).startswith('gesher/nsd/') for value in class_files):
        raise RuntimeError('Reference unexpectedly defines a platform/provider class')
    class_jar = output / 'reference-classes.jar'
    archive(class_jar, [(str(value.relative_to(classes)), value.read_bytes()) for value in class_files])
    commands.append([str(args.java_home / 'bin/java'), '-Xmx1g', '-cp', str(args.d8_jar),
                     'com.android.tools.r8.D8', '--min-api', '36', '--lib', str(args.android_jar),
                     '--output', str(dex), str(class_jar)])
    result = subprocess.run(commands[-1], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (output / 'd8.log').write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(result.stdout)
    payload = output / 'reference.jar'
    archive(payload, [(value.name, value.read_bytes()) for value in sorted(dex.glob('*.dex'))])
    remote = '/data/local/tmp/gesher-nsd-resolve-reference-' + digest(frozen)[:12]
    remote_jar = remote + '/reference.jar'
    setup = {'description': 'Install immutable stock SDK36 NSD binary-resolution reference', 'steps': [
        {'adb': ['shell', 'mkdir', '-p', remote], 'timeout': 30},
        {'adb': ['push', str(payload), remote_jar], 'timeout': 90},
        {'adb': ['shell', 'chmod', '0444', remote_jar], 'timeout': 30},
        {'adb': ['shell', 'sha256sum', remote_jar], 'timeout': 30}]}
    run = {'description': 'Stock API36 NSD resolution from an owned independent raw wire peer', 'steps': [
        {'adb': ['shell', 'env', 'CLASSPATH=' + remote_jar, 'app_process', '/system/bin',
                 'gesher.nsd.NsdResolveAndroidReference'], 'timeout': 120}]}
    (output / 'setup-request.json').write_text(json.dumps(setup, indent=2) + '\n')
    (output / 'run-request.json').write_text(json.dumps(run, indent=2) + '\n')
    manifest = {'scope': 'Stock Android API36 IPv4 NSD wire-resolution reference; no Windows execution or ordinary APK permission claim',
                'source': {'repositoryPath': str(source.relative_to(ROOT)), 'snapshot': str(frozen), 'sha256': digest(frozen)},
                'builderSha256': digest(Path(__file__)), 'compilationSdk': {'path': str(args.android_jar), 'sha256': digest(args.android_jar)},
                'd8': {'path': str(args.d8_jar), 'sha256': digest(args.d8_jar)},
                'classesJarSha256': digest(class_jar), 'payload': {'path': str(payload), 'sha256': digest(payload), 'devicePath': remote_jar},
                'classes': {str(value.relative_to(classes)): digest(value) for value in class_files},
                'dex': {value.name: digest(value) for value in sorted(dex.glob('*.dex'))},
                'providerBoundary': 'Payload defines gesher.nsd only. Runtime assertions require platform NsdManager/NsdServiceInfo ownership; no replaced platform classes or flag overrides.',
                'commands': commands, 'setupSha256': digest(output / 'setup-request.json'), 'runSha256': digest(output / 'run-request.json')}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'manifest': str(output / 'manifest.json'), 'manifestSha256': digest(output / 'manifest.json'),
                      'payloadSha256': digest(payload), 'sourceSha256': digest(frozen), 'classes': len(class_files)}))


if __name__ == '__main__':
    main()
