#!/usr/bin/env python3
"""Execute production sync admission, results, clock and cancellation contracts.

Use --source-ref with --case admission/clock/results/cancellation for separate negative
controls. The same Java fixture runs generated Binder/Parcel calls on Windows;
this host path uses local service calls and the declared existing native adapter.
--idle-policy-probe uses production classes only from --app-classes, which must be
the full application built after combining this change with PR15's idle policy.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--framework-classes', type=Path, required=True)
    parser.add_argument('--core-classes', type=Path, required=True)
    parser.add_argument('--app-classes', type=Path, required=True)
    parser.add_argument('--apksig-jar', type=Path, required=True)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--source-ref', help='Compile older production classes as a negative control')
    source.add_argument('--idle-policy-probe', action='store_true',
                        help='Probe the full PR15 combination supplied through --app-classes')
    parser.add_argument('--case', choices=('all', 'admission', 'clock', 'results', 'cancellation'), default='all')
    args = parser.parse_args()
    if args.idle_policy_probe and args.case != 'all':
        parser.error('--idle-policy-probe requires --case all')
    cc = shutil.which('cc')
    if not cc:
        parser.error('A C compiler is required for the declared native boundaries')
    classpath = os.pathsep.join(str(p.resolve()) for p in
                               (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    with tempfile.TemporaryDirectory(prefix='gesher-sync-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'
        classes.mkdir()
        library = work / 'libsync_host.so'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/sync/framework_host.c'), '-o', str(library)], check=True)
        sources = []
        # The explicit coexistence probe must use the complete supplied merged app,
        # without substituting current or baseline production classes over its jar.
        for name in (() if args.idle_policy_probe else ('SyncManager',)):
            relative = 'app/java/com/gesher/server/' + name + '.java'
            source = ROOT / relative
            if args.source_ref:
                before = subprocess.run(['git', 'show', args.source_ref + ':' + relative],
                                        cwd=ROOT, capture_output=True, check=True)
                source = work / (name + '.java')
                source.write_bytes(before.stdout)
            sources.append(str(source))
        main_class = 'SyncIdlePr15Probe' if args.idle_policy_probe else 'SyncManagerHost'
        sources.extend(str(ROOT / p) for p in (
            'tests/dex-services/gesher/services/SyncManagerContractTest.java',
            'tests/sync/' + main_class + '.java'))
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', classpath, '-d', str(classes), *sources], check=True)
        cleaner = work / 'cleaner' / 'sun' / 'misc'
        cleaner.mkdir(parents=True)
        for source in (args.core_classes / 'sun/misc').glob('Cleaner*.class'):
            shutil.copy2(source, cleaner / source.name)
        environment = os.environ.copy()
        environment['GESHER_MICROG_SPOOF'] = '1'
        environment['ANDROID_DATA'] = str(work / 'data')
        environment.pop('GESHER_BINDER_UID', None)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                        '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
                        '--patch-module', 'jdk.unsupported=' + str(work / 'cleaner'),
                        '-cp', str(classes) + os.pathsep + classpath,
                        main_class, str(library), args.case],
                       env=environment, check=True, timeout=90)

if __name__ == '__main__':
    main()
