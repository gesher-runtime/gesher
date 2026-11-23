#!/usr/bin/env python3
"""Run exact-alarm policy, AppOps, Settings and process-lifecycle Java contracts.

Uses actual production services/framework classes with the existing declared
host JNI identity/allocation boundaries. Windows track 94 exercises Binder
parcels; the native permission-dialog test verifies the actual Settings controls.
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
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc:
        parser.error('A C compiler is required for the declared native boundaries')
    classpath = os.pathsep.join(str(p.resolve()) for p in
                               (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    with tempfile.TemporaryDirectory(prefix='gesher-exact-alarm-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'
        classes.mkdir()
        library = work / 'libexact_alarm_host.so'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/scheduler/framework_host.c'), '-o', str(library)], check=True)
        sources = [str(ROOT / p) for p in (
            'app/java/com/gesher/server/AlarmManagerService.java',
            'app/java/com/gesher/server/ExactAlarmAccess.java',
            'app/java/com/gesher/server/ExactAlarmAccessRequests.java',
            'app/java/com/gesher/server/AccountChooser.java',
            'app/java/com/gesher/server/PermissionPolicy.java',
            'app/java/com/gesher/server/PermissionCatalog.java',
            'app/java/com/gesher/server/ActivityManagerService.java',
            'app/java/com/gesher/server/ActivityTaskManagerService.java',
            'tests/dex-services/gesher/services/ExactAlarmContractTest.java',
            'tests/exact-alarm/ExactAlarmHost.java')]
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
                        'ExactAlarmHost', str(library)],
                       env=environment, check=True, timeout=90)

if __name__ == '__main__':
    main()
