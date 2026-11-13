#!/usr/bin/env python3
"""Execute real scheduler, job parameter and alarm contracts against host native boundaries.

Use --source-ref with --case drain/stopping/race/alarms for separate negative
controls. The same Java fixture runs generated Binder/Parcel calls on Windows;
this host path uses local service calls and the declared existing native adapter.
The delivery case also exercises bounded alarm/broadcast ownership, with local
listener death notifications and manual timer boundaries on both platforms.
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
    parser.add_argument('--source-ref', help='Compile older production classes as a negative control')
    parser.add_argument('--case', choices=('all', 'drain', 'stopping', 'race', 'alarms', 'delivery'), default='all')
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc:
        parser.error('A C compiler is required for the declared native boundaries')
    classpath = os.pathsep.join(str(p.resolve()) for p in
                               (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    with tempfile.TemporaryDirectory(prefix='gesher-scheduler-work-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'
        classes.mkdir()
        library = work / 'libscheduler_host.so'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/scheduler/framework_host.c'), '-o', str(library)], check=True)
        sources = []
        for name in ('JobSchedulerService', 'AlarmManagerService', 'BroadcastController', 'DeviceWork'):
            relative = 'app/java/com/gesher/server/' + name + '.java'
            source = ROOT / relative
            if args.source_ref:
                before = subprocess.run(['git', 'show', args.source_ref + ':' + relative],
                                        cwd=ROOT, capture_output=True, check=True)
                source = work / (name + '.java')
                source.write_bytes(before.stdout)
            sources.append(str(source))
        sources.extend(str(ROOT / p) for p in (
            'tests/dex-services/gesher/services/SchedulerWorkContractTest.java',
            'tests/scheduler/SchedulerWorkHost.java'))
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
                        'SchedulerWorkHost', str(library), args.case],
                       env=environment, check=True, timeout=90)

if __name__ == '__main__':
    main()
