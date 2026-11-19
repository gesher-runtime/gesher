#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Fresh genuine NSD subscription policy/core fixtures with explicit host boundaries.

This executes neither Windows sockets nor remote Binder. The supplied provider
classes must come from the complete app compilation against genuine framework
and Android core inputs; the SDK public jar is never a runtime provider.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time
import secrets

ROOT = Path(__file__).resolve().parents[1]
SUITES = {
    'state': ('com.gesher.net.mdns.NsdSubscriptionStateTest', 'NsdSubscriptionStateTest.java'),
    'manager': ('com.gesher.net.NsdSubscriptionManagerClientTest', 'NsdSubscriptionManagerClientTest.java'),
    'service': ('com.gesher.server.NsdSubscriptionServiceHost', 'NsdSubscriptionServiceHost.java'),
    'worker': ('com.gesher.net.NsdSubscriptionWorkerTest', 'NsdSubscriptionWorkerTest.java'),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'app-classes', 'framework-classes', 'core-classes', 'apksig-jar', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--suite', choices=list(SUITES), action='append')
    parser.add_argument('--loopback-peers', action='store_true',
                        help='Also exercise the production worker against owned IPv4/IPv6 UDP peers')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    providers = ('com/gesher/net/mdns/MdnsResponse.class', 'com/gesher/net/mdns/MdnsResponseDecoder.class',
                 'com/gesher/net/mdns/MdnsQueryScheduler.class', 'com/gesher/net/mdns/MdnsSubscriptionState.class',
                 'com/gesher/net/WindowsNsdSubscriber.class', 'com/gesher/net/WindowsNsdResolver.class',
                 'com/gesher/net/NsdSubscriptionClient.class', 'com/gesher/server/NsdSubscriptionService.class',
                 'com/gesher/server/Services.class', 'android/net/nsd/NsdManager.class')
    for name in providers:
        if not (args.app_classes / name).is_file():
            raise ValueError('Full genuine provider compilation required: ' + name)
    fixtures = [ROOT / 'tests/nsd' / value[1] for value in SUITES.values()]
    fixtures += [ROOT / 'tests/nsd/NsdRegistrationServiceHost.java']
    sources = [p for p in (ROOT / 'app/java').rglob('*.java') if '/com/google/' not in str(p)]
    sources += fixtures + [Path(__file__), ROOT / 'tests/nsd/subscription_peer.py',
                           ROOT / 'tests/nsd/resolver_peer.py', ROOT / 'tests/nsd/peer_scope.py']
    source_hashes = {str(path): sha(path) for path in sources}
    class_hashes = {str(path): sha(path) for path in args.app_classes.rglob('*.class')}
    (out / 'initial-source-hashes.json').write_text(json.dumps(source_hashes, indent=2) + '\n')
    (out / 'initial-class-hashes.json').write_text(json.dumps(class_hashes, indent=2) + '\n')
    commands, observations = [], []
    started = time.time()

    def run(command, label, env=None):
        commands.append(command)
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        begin = time.monotonic()
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=90, env=env)
        (out / (label + '.log')).write_text(result.stdout)
        observations.append({'label': label, 'exit': result.returncode, 'seconds': time.monotonic() - begin})
        (out / 'observations.json').write_text(json.dumps(observations, indent=2) + '\n')
        print(result.stdout, end='')
        if result.returncode:
            raise RuntimeError(label + ' failed')

    library = out / 'libnsd_subscription_host.so'
    dependencies = out / 'native.d'
    run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC', '-MD', '-MF',
         str(dependencies), '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
         str(ROOT / 'tests/location/framework_host.c'), '-o', str(library)], 'host-native')
    cp = os.pathsep.join(str(path.resolve()) for path in
                        (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    classes = out / 'classes'
    run([str(args.java_home / 'bin/javac'), '-J-Xmx700m', '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
         '-cp', cp, '-d', str(classes), *map(str, fixtures)], 'compile-fixtures')
    cleaner = out / 'cleaner/sun/misc'
    cleaner.mkdir(parents=True)
    cleaner_inputs = list((args.core_classes / 'sun/misc').glob('Cleaner*.class'))
    if not cleaner_inputs:
        raise ValueError('Actual Android Cleaner input is required for host Binder holder boundary')
    for path in cleaner_inputs:
        shutil.copy2(path, cleaner / path.name)
    environment = os.environ.copy()
    environment['ANDROID_DATA'] = str(out / 'data')
    environment['GESHER_MICROG_SPOOF'] = '0'
    java = [str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-XX:+UnlockDiagnosticVMOptions',
            '-XX:-BytecodeVerificationRemote', '--patch-module', 'jdk.unsupported=' + str(out / 'cleaner'),
            '-cp', str(classes) + os.pathsep + cp]
    wire = out / 'wire'
    wire.mkdir()
    run([sys.executable, str(ROOT / 'tests/nsd/subscription_peer.py'), '--fixtures',
         str(wire / 'subscription-fixtures.tsv')], 'independent-valid-packets')
    for name in args.suite or SUITES:
        run(java + [SUITES[name][0], str(library), str(out / 'wire')], name, environment)
    peer_observations = []
    if args.loopback_peers:
        for family, address in ((4, '127.0.0.1'), (6, '::1')):
            control = out / ('peer-ipv' + str(family))
            service_type = '_gr' + secrets.token_hex(6) + '._tcp'
            instance = 'Owned.שלום\\😀'
            # The fixture driver obtains the actual loopback interface index;
            # Python's loopback bind does not require an IPv6 interface scope.
            command = [sys.executable, str(ROOT / 'tests/nsd/subscription_peer.py'),
                       '--control', str(control), '--type', service_type, '--instance', instance,
                       '--family', str(family), '--address', address, '--index', '0', '--loopback']
            commands.append(command)
            peer_log = out / ('peer-ipv' + str(family) + '.log')
            with peer_log.open('w') as stream:
                peer = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 8
                    while not (control / 'ready').exists() and peer.poll() is None and time.monotonic() < deadline:
                        time.sleep(.02)
                    if not (control / 'ready').exists():
                        raise RuntimeError('Owned IPv%d peer failed to start: %s' % (family, peer_log.read_text()))
                    run(java + [SUITES['worker'][0], str(library), 'loopback', str(control),
                                service_type, instance, str(family)], 'loopback-ipv' + str(family), environment)
                    peer.wait(timeout=5)
                    if peer.returncode:
                        raise RuntimeError('Owned peer failed: ' + peer_log.read_text())
                    peer_observations.append(json.loads((control / 'result.json').read_text()))
                finally:
                    if peer.poll() is None:
                        (control / 'command').write_text('999 quit')
                        try:
                            peer.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            peer.terminate()
                            peer.wait(timeout=3)
    includes = [Path(value).resolve() for value in shlex.split(
        dependencies.read_text().replace('\\\n', '').partition(':')[2])]
    if source_hashes != {str(path): sha(path) for path in sources}:
        raise RuntimeError('Source changed during behavioral qualification')
    if class_hashes != {str(path): sha(path) for path in args.app_classes.rglob('*.class')}:
        raise RuntimeError('Provider classes changed during behavioral qualification')
    (out / 'manifest.json').write_text(json.dumps({
        'startedEpochSeconds': started, 'commands': commands, 'observations': observations,
        'sourceHashes': source_hashes, 'providerClassHashes': class_hashes,
        'fixtureClassHashes': {str(path): sha(path) for path in classes.rglob('*.class')},
        'hostNativeIncludes': {str(path): sha(path) for path in includes},
        'hostNativeLibrarySha256': sha(library), 'apksigSha256': sha(args.apksig_jar),
        'androidCleanerInputs': {str(path): sha(path) for path in cleaner_inputs},
        'wireHashes': {str(path): sha(path) for path in (out / 'wire').glob('*')},
        'ownedLoopbackPeers': peer_observations,
        'windowsExecution': False, 'remoteBinderExecution': False, 'nativeSocketExecution': False,
        'explicitBoundaries': ['host framework initialization and clock', 'app executor and service lookup',
                               'Binder holder/lifetime', 'parsed package and permission metadata',
                               'native process identity', 'serial dispatch and bounded retry',
                               'protocol backend', 'unrelated locale settings',
                               'controlled actual private acquire/enroll interval for finite concurrency']
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
