#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Run genuine registration service policy with explicit host lifecycle boundaries.

Supplied app classes must come from a fresh complete production compilation,
including WindowsNsdAdvertiser. This does not substitute a production provider or
execute remote Binder, ART or the Windows transport.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes', 'apksig-jar', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=False)
    for provider in ('com/gesher/server/NsdRegistrationService.class',
                     'com/gesher/net/WindowsNsdAdvertiser.class'):
        if not (args.app_classes / provider).is_file():
            raise ValueError('Complete genuine production class directory required: ' + provider)
    initial = [p for p in (ROOT / 'app/java').rglob('*.java')
               if not p.is_relative_to(ROOT / 'app/java/com/google')]
    initial += [ROOT / 'tests/nsd/NsdRegistrationServiceHost.java', Path(__file__)]
    (work / 'initial-source-hashes.json').write_text(json.dumps({
        str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in initial}, indent=2) + '\n')
    (work / 'initial-class-hashes.json').write_text(json.dumps({
        str(p): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in args.app_classes.rglob('*.class')}, indent=2) + '\n')
    cp = os.pathsep.join(str(value.resolve()) for value in
                        (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    commands = []
    observations = []
    started = time.time()

    def run(command, label, env=None):
        commands.append(command)
        (work / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        begin = time.monotonic()
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=90, env=env)
        (work / (label + '.log')).write_text(result.stdout)
        observations.append({'label': label, 'exitCode': result.returncode,
                             'seconds': time.monotonic() - begin})
        (work / 'observations.json').write_text(json.dumps(observations, indent=2) + '\n')
        print(result.stdout, end='')
        if result.returncode:
            raise RuntimeError(label + ' exited ' + str(result.returncode))

    native_source = ROOT / 'tests/location/framework_host.c'
    depfile = work / 'host-native.d'
    library = work / 'libnsd_registration_service_host.so'
    fixture = ROOT / 'tests/nsd/NsdRegistrationServiceHost.java'
    classes = work / 'classes'
    run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC', '-MMD', '-MF',
         str(depfile), '-I' + str(args.java_home / 'include'),
         '-I' + str(args.java_home / 'include/linux'), str(native_source), '-o', str(library)],
        'host-native')
    run([str(args.java_home / 'bin/javac'), '-J-Xmx700m', '-J-XX:-UsePerfData', '-encoding',
         'UTF-8', '-cp', cp, '-d', str(classes), str(fixture)], 'compile-fixture')
    cleaner = work / 'cleaner' / 'sun/misc'
    cleaner.mkdir(parents=True)
    cleaner_inputs = list((args.core_classes / 'sun/misc').glob('Cleaner*.class'))
    if not cleaner_inputs:
        raise ValueError('The genuine Android core class input has no Cleaner classes')
    for entry in cleaner_inputs:
        shutil.copy2(entry, cleaner / entry.name)
    environment = os.environ.copy()
    environment['ANDROID_DATA'] = str(work / 'data')
    environment['GESHER_MICROG_SPOOF'] = '0'
    run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '--patch-module',
         'jdk.unsupported=' + str(work / 'cleaner'), '-cp', str(classes) + os.pathsep + cp,
         'com.gesher.server.NsdRegistrationServiceHost', str(library)], 'contracts', environment)
    sources = [ROOT / p for p in (
        'app/java/com/gesher/server/NsdRegistrationService.java',
        'app/java/com/gesher/server/ActivityManagerService.java',
        'app/java/com/gesher/server/Services.java',
        'app/java/com/gesher/server/CallerIdentity.java',
        'app/java/com/gesher/server/PermissionPolicy.java',
        'app/java/com/gesher/server/ActivityTaskManagerService.java',
        'app/java/com/gesher/server/PackageManagerService.java',
        'app/java/com/gesher/net/INsdRegistration.java',
        'app/java/com/gesher/net/NsdRegistrationClient.java',
        'app/java/com/gesher/net/NsdRegistrationContract.java',
        'app/java/com/gesher/net/WindowsNsdAdvertiser.java')]
    sources += [fixture, Path(__file__)]
    included = [Path(value).resolve() for value in shlex.split(
        depfile.read_text().replace('\\\n', '').partition(':')[2])]
    hashes = lambda values: {str(value): hashlib.sha256(value.read_bytes()).hexdigest()
                            for value in values}
    class_inputs = []
    for folder, patterns in (
            (args.app_classes, ['com/gesher/server/NsdRegistrationService*.class',
                               'com/gesher/server/ActivityManagerService*.class',
                               'com/gesher/server/ActivityTaskManagerService*.class',
                               'com/gesher/server/CallerIdentity*.class',
                               'com/gesher/server/PermissionPolicy*.class',
                               'com/gesher/server/PackageManagerService*.class',
                               'com/gesher/net/INsdRegistration*.class',
                               'com/gesher/net/NsdRegistrationClient*.class',
                               'com/gesher/net/NsdRegistrationContract*.class',
                               'com/gesher/net/WindowsNsdAdvertiser*.class']),
            (classes, ['**/*.class'])):
        for pattern in patterns:
            class_inputs.extend(folder.glob(pattern))
    (work / 'manifest.json').write_text(json.dumps({
        'startedEpochSeconds': started, 'commands': commands, 'observations': observations,
        'sourceHashes': hashes(sources), 'classHashes': hashes(class_inputs),
        'hostNativeIncludeHashes': hashes(included),
        'androidCleanerInputHashes': hashes(cleaner_inputs),
        'hostNativeLibrarySha256': hashlib.sha256(library.read_bytes()).hexdigest(),
        'apksigInput': hashes([args.apksig_jar]),
        'windowsExecution': False, 'remoteBinderExecution': False,
        'explicitBoundaries': ['parsed package/framework-permission metadata', 'native PID/UID',
                               'Binder lifetime', 'serial dispatch and delayed cleanup retry',
                               'already-connected client service boundary', 'advertiser backend',
                               'unrelated device locale settings']}, indent=2) + '\n')


if __name__ == '__main__':
    main()
