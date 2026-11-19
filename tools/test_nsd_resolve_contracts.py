#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Qualify production NSD resolver state against explicit, independently encoded fixtures.

Uses genuine Android framework/core/app classes, with a narrow host native
initialization boundary. This tool does not execute the Windows native transport.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=False)
    cp = os.pathsep.join(str(value.resolve()) for value in
                        (args.app_classes, args.framework_classes, args.core_classes))
    library = work / 'libnsd_resolve_host.so'
    commands = []

    def run(command, label):
        commands.append(command)
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=90)
        (work / (label + '.log')).write_text(result.stdout)
        print(result.stdout, end='')
        if result.returncode:
            raise RuntimeError(label + ' exited ' + str(result.returncode))

    dependency_file = work / 'host-native.d'
    run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
         '-MMD', '-MF', str(dependency_file),
         '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
         str(ROOT / 'tests/nsd/framework_host.c'), '-o', str(library)], 'host-native')
    fixture = work / 'valid-packets.tsv'
    run(['python3', str(ROOT / 'tests/nsd/resolver_peer.py'), '--fixtures', str(fixture)], 'peer-fixtures')
    sources = list((ROOT / 'app/java/com/gesher/net/mdns').glob('*.java'))
    sources += [ROOT / 'app/java/com/gesher/net/WindowsNsdResolver.java',
                ROOT / 'app/java/com/gesher/net/WindowsNsd.java',
                ROOT / 'app/java/android/net/nsd/NsdManager.java',
                ROOT / 'tests/dex-services/gesher/services/NsdResolveContractTest.java',
                ROOT / 'tests/dex-services/gesher/services/NsdResolvePeerTest.java']
    classes = work / 'classes'
    run([str(args.java_home / 'bin/javac'), '-J-Xmx1g', '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
         '-cp', cp, '-d', str(classes), *map(str, sources)], 'compile')
    run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-XX:+UnlockDiagnosticVMOptions',
         '-XX:-BytecodeVerificationRemote', '-cp', str(classes) + os.pathsep + cp,
         'gesher.services.NsdResolveContractTest', str(library), str(fixture)], 'contracts')
    for family in (4, 6):
        control = work / ('peer-ipv' + str(family))
        control.mkdir()
        service_type = '_gr' + uuid.uuid4().hex[:12] + '._tcp'
        instance = 'Owned.שלום\\😀'
        command = ['python3', str(ROOT / 'tests/nsd/resolver_peer.py'), '--control', str(control),
                   '--type', service_type, '--instance', instance, '--index', '0', '--address',
                   '127.0.0.1' if family == 4 else '::1', '--family', str(family), '--loopback']
        commands.append(command)
        with (control / 'peer.log').open('w') as output:
            peer = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 10
                while not (control / 'ready').exists():
                    if peer.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('Independent IPv' + str(family) + ' peer failed: ' + (control / 'peer.log').read_text())
                    time.sleep(.02)
                run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-XX:+UnlockDiagnosticVMOptions',
                     '-XX:-BytecodeVerificationRemote', '-cp', str(classes) + os.pathsep + cp,
                     'gesher.services.NsdResolvePeerTest', str(library), str(control), str(family),
                     service_type, instance], 'peer-ipv' + str(family))
            finally:
                (control / 'command-new').write_text('1 quit')
                (control / 'command-new').replace(control / 'command')
                try:
                    peer.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    peer.terminate()
                    peer.wait(timeout=5)
            if peer.returncode:
                raise RuntimeError('Peer exit ' + str(peer.returncode))
    inputs = {str(value.relative_to(ROOT)): hashlib.sha256(value.read_bytes()).hexdigest()
              for value in sources + [ROOT / 'tests/nsd/resolver_peer.py', ROOT / 'tests/nsd/peer_scope.py',
                                       ROOT / 'tests/nsd/framework_host.c', Path(__file__)]}
    included = [Path(value).resolve() for value in shlex.split(
        dependency_file.read_text().replace('\\\n', '').partition(':')[2])]
    native_inputs = {str(value): hashlib.sha256(value.read_bytes()).hexdigest() for value in included}
    (work / 'manifest.json').write_text(json.dumps({'sourceHashes': inputs, 'commands': commands,
            'hostNativeIncludeHashes': native_inputs,
            'hostNativeLibrarySha256': hashlib.sha256(library.read_bytes()).hexdigest(),
            'fixtureSha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
            'windowsExecution': False}, indent=2) + '\n')


if __name__ == '__main__':
    main()
