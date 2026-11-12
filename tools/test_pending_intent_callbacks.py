#!/usr/bin/env python3
"""Exercise real PendingIntent/clipboard services and framework classes with declared JNI boundaries.

Use --source-ref HEAD with --case identity/fill/update/cancel/completion/public/clipboard
to demonstrate the corresponding baseline regression. Windows dex mains additionally
exercise real Parcel/Binder transport and native clipboard events.
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
    parser.add_argument('--case', choices=('all', 'identity', 'fill', 'update', 'cancel', 'completion', 'public', 'clipboard'), default='all')
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc: parser.error('A C compiler is required for the declared native boundaries')
    classpath = os.pathsep.join(str(p.resolve()) for p in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    with tempfile.TemporaryDirectory(prefix='gesher-pending-callbacks-') as temporary:
        work = Path(temporary); classes = work / 'classes'; classes.mkdir()
        library = work / 'libcallback_host.so'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/pending-intent/framework_host.c'), '-o', str(library)], check=True)
        sources = []
        for name in ('PendingIntents', 'ActivityManagerService', 'BroadcastController', 'ClipboardService'):
            relative = 'app/java/com/gesher/server/' + name + '.java'
            source = ROOT / relative
            if args.source_ref:
                before = subprocess.run(['git', 'show', args.source_ref + ':' + relative], cwd=ROOT,
                                        capture_output=True, check=True)
                source = work / (name + '.java'); source.write_bytes(before.stdout)
            sources.append(str(source))
        sources.extend(str(ROOT / p) for p in (
            'tests/dex-services/gesher/services/PendingIntentContractTest.java',
            'tests/dex-services/gesher/services/ClipboardCallbackTest.java',
            'tests/dex-services/gesher/services/PowerWakeLockCallbackTest.java',
            'tests/pending-intent/CallbackContractsHost.java'))
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', classpath, '-d', str(classes), *sources], check=True)
        # Binder's real NativeAllocationRegistry uses Android's Cleaner. Put those
        # exact supplied libcore classes in their owning JDK module, without replacements.
        cleaner = work / 'cleaner' / 'sun' / 'misc'; cleaner.mkdir(parents=True)
        for source in (args.core_classes / 'sun/misc').glob('Cleaner*.class'):
            shutil.copy2(source, cleaner / source.name)
        environment = os.environ.copy(); environment['GESHER_MICROG_SPOOF'] = '1'
        environment.pop('GESHER_BINDER_UID', None)
        cases = (args.case, 'clipboard') if args.case == 'all' else (args.case,)
        for case in cases:
            subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                            '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
                            '--patch-module', 'jdk.unsupported=' + str(work / 'cleaner'),
                            '-cp', str(classes) + os.pathsep + classpath,
                            'CallbackContractsHost', str(library), case], env=environment, check=True, timeout=90)

if __name__ == '__main__': main()
