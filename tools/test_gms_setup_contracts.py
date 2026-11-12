#!/usr/bin/env python3
"""Compile production account/ATMS services and run bounded setup lifecycle contracts."""
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
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes', 'apksig-jar'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    classpath = os.pathsep.join(str(p.resolve()) for p in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    with tempfile.TemporaryDirectory(prefix='gesher-gms-setup-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'; classes.mkdir()
        library = work / 'libgms_setup_host.so'
        subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
            '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
            str(ROOT / 'tests/gms-setup/framework_host.c'), '-o', str(library)], check=True)
        sources = [ROOT / p for p in (
            'app/java/com/gesher/server/AccountManagerService.java',
            'app/java/com/gesher/server/ActivityTaskManagerService.java',
            'tests/dex-services/gesher/services/GmsSetupContractTest.java',
            'tests/gms-setup/GmsSetupHost.java')]
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
            '-cp', classpath, '-d', str(classes), *map(str, sources)], check=True)
        cleaner = work / 'cleaner/sun/misc'; cleaner.mkdir(parents=True)
        for source in (args.core_classes / 'sun/misc').glob('Cleaner*.class'):
            shutil.copy2(source, cleaner / source.name)
        environment = os.environ.copy(); environment['ANDROID_DATA'] = str(work / 'data')
        environment['GESHER_MICROG_SPOOF'] = '1'; environment.pop('GESHER_BINDER_UID', None)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
            '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
            '--patch-module', 'jdk.unsupported=' + str(work / 'cleaner'),
            '-cp', str(classes) + os.pathsep + classpath, 'GmsSetupHost', str(library)],
            env=environment, check=True, timeout=60)

if __name__ == '__main__':
    main()
