#!/usr/bin/env python3
"""Exercise real PendingIntent/clipboard services and framework classes with declared JNI boundaries.

Use --source-ref HEAD with --case identity/fill/update/cancel/completion/public/clipboard
to demonstrate the corresponding baseline regression. Windows dex mains additionally
exercise real Parcel/Binder transport and native clipboard events.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from content_observer_reference import prepare as prepare_content_reference

ROOT = Path(__file__).resolve().parents[1]

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--framework-classes', type=Path, required=True)
    parser.add_argument('--core-classes', type=Path, required=True)
    parser.add_argument('--app-classes', type=Path, required=True)
    parser.add_argument('--apksig-jar', type=Path, required=True)
    parser.add_argument('--source-ref', help='Compile older production classes as a negative control')
    parser.add_argument('--case', choices=('all', 'identity', 'fill', 'update', 'cancel', 'completion', 'public', 'clipboard', 'content', 'content-work', 'content-reference'), default='all')
    parser.add_argument('--content-work-kind', choices=('selective', 'miss', 'broad', 'ancestors'), default='selective')
    parser.add_argument('--content-work-observers', type=int, default=4096)
    parser.add_argument('--content-work-iterations', type=int, default=2000)
    parser.add_argument('--content-reference-source', type=Path,
                        default=ROOT / 'third_party/aosp/frameworks/base/services/core/java/com/android/server/content/ContentService.java',
                        help='Pinned Android 16 r4 source for independent observer selection comparison')
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
        for name in ('PendingIntents', 'ActivityManagerService', 'BroadcastController', 'ContentService'):
            relative = 'app/java/com/gesher/server/' + name + '.java'
            source = ROOT / relative
            if args.source_ref:
                before = subprocess.run(['git', 'show', args.source_ref + ':' + relative], cwd=ROOT,
                                        capture_output=True, check=True)
                source = work / (name + '.java'); source.write_bytes(before.stdout)
            if name == 'ContentService':
                print('ContentService SHA256 ' + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
            sources.append(str(source))
        sources.extend(str(ROOT / p) for p in (
            'tests/dex-services/gesher/services/PendingIntentContractTest.java',
            'tests/dex-services/gesher/services/ContentObserverLifecycleTest.java',
            'tests/dex-services/gesher/services/ContentObserverDispatchTest.java',
            'tests/java-performance/ContentObserverDispatchHost.java',
            'tests/dex-services/gesher/services/PowerWakeLockCallbackTest.java',
            'tests/pending-intent/CallbackContractsHost.java'))
        reference = args.case in ('all', 'content', 'content-reference') and args.content_reference_source.is_file()
        if args.case == 'content-reference' and not reference:
            parser.error('--content-reference-source must name the pinned AOSP source')
        if reference:
            sources.append(str(prepare_content_reference(args.content_reference_source,
                                                        work / 'ContentObserverReferenceHost.java')))
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', classpath, '-d', str(classes), *sources], check=True)
        # Binder's real NativeAllocationRegistry uses Android's Cleaner. Put those
        # exact supplied libcore classes in their owning JDK module, without replacements.
        cleaner = work / 'cleaner' / 'sun' / 'misc'; cleaner.mkdir(parents=True)
        for source in (args.core_classes / 'sun/misc').glob('Cleaner*.class'):
            shutil.copy2(source, cleaner / source.name)
        environment = os.environ.copy(); environment['GESHER_MICROG_SPOOF'] = '1'
        environment.pop('GESHER_BINDER_UID', None)
        cases = (args.case, 'clipboard', 'content') if args.case == 'all' else (args.case,)
        for case in cases:
            if case == 'clipboard':
                # Clipboard now authenticates actual PM/ATM identities and AppOps. Its
                # dedicated fixture supplies the required native clock/identity boundaries.
                subprocess.run([sys.executable, str(ROOT / 'tools/test_clipboard.py'),
                                '--java-home', str(args.java_home),
                                '--framework-classes', str(args.framework_classes),
                                '--core-classes', str(args.core_classes),
                                '--app-classes', str(args.app_classes),
                                '--apksig', str(args.apksig_jar)], check=True, timeout=90)
                continue
            case_environment = environment.copy()
            # PMS now persists enabled-state ownership during construction. Keep
            # every host case off the device's /data and isolated from other cases.
            case_environment['ANDROID_DATA'] = str(work / ('data-' + case))
            if case in ('content', 'content-work', 'content-reference'): case_environment.pop('GESHER_MICROG_SPOOF', None)
            if case == 'content-work':
                main = ['ContentObserverDispatchHost', str(library), str(args.content_work_observers),
                        args.content_work_kind, str(args.content_work_iterations)]
            elif case == 'content-reference':
                main = ['gesher.services.ContentObserverReferenceHost', str(library)]
            else:
                main = ['CallbackContractsHost', str(library), case]
            subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                            *(['-Xbatch', '-Xmx256m'] if case == 'content-work' else []),
                            '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
                            '--patch-module', 'jdk.unsupported=' + str(work / 'cleaner'),
                            '-cp', str(classes) + os.pathsep + classpath,
                            *main], env=case_environment, check=True, timeout=90)
            if case == 'content':
                if reference:
                    subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                                    '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
                                    '--patch-module', 'jdk.unsupported=' + str(work / 'cleaner'),
                                    '-cp', str(classes) + os.pathsep + classpath,
                                    'gesher.services.ContentObserverReferenceHost', str(library)],
                                   env=case_environment, check=True, timeout=90)
                else:
                    print('Android observer source comparison not run: supply --content-reference-source', flush=True)

if __name__ == '__main__': main()
