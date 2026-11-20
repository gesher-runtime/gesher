#!/usr/bin/env python3
"""Run the production broker and SpeechRecognizer client against local fixtures.

By default use the built framework classes. When checking against an older build
of those classes, use --unpatched-aosp-base to compile the patched client from
the pinned pre-patch frameworks/base checkout.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CLIENT = Path('core/java/android/speech/SpeechRecognizerImpl.java')
CLIENT_PATCH = ROOT / 'patches/frameworks/base/0784-speech-recognition-connection-lifecycle.patch'


def build_native(cc, java_home, source, output):
    subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC', '-pthread',
                    '-I' + str(java_home / 'include'), '-I' + str(java_home / 'include/linux'),
                    str(ROOT / 'tests' / source / 'framework_host.c'), '-o', str(output)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ('java-home', 'framework-classes', 'core-classes', 'app-classes'):
        parser.add_argument('--' + argument, type=Path, required=True)
    parser.add_argument('--unpatched-aosp-base', type=Path)
    parser.add_argument('--source-ref', help='Use an older broker as a dispatch-work negative control')
    args = parser.parse_args()
    classpath = os.pathsep.join(str(p.resolve()) for p in
                                (args.app_classes, args.framework_classes, args.core_classes))
    cc = shutil.which('cc')
    if cc is None:
        parser.error('a native C compiler is required')
    with tempfile.TemporaryDirectory(prefix='gesher-speech-') as temporary:
        work = Path(temporary)
        binder = work / 'libspeech_binder_host.so'
        looper = work / 'libspeech_looper_host.so'
        build_native(cc, args.java_home, 'scheduler', binder)
        build_native(cc, args.java_home, 'cookie-callbacks', looper)
        broker_path = 'app/java/com/gesher/server/SpeechRecognitionManagerService.java'
        broker_text = (subprocess.run(['git', 'show', args.source_ref + ':' + broker_path],
                                     cwd=ROOT, capture_output=True, text=True, check=True).stdout
                       if args.source_ref else (ROOT / broker_path).read_text())
        # Count actual visits to request dispatch in the complete production
        # broker, without shipping test counters or relying on host timing.
        counter_anchor = '    private static final int MAX_CONNECTIONS_PER_UID = 10;'
        dispatch_anchor = '            final boolean dispatch(IRecognitionService provider) {'
        if broker_text.count(counter_anchor) != 1 or broker_text.count(dispatch_anchor) != 1:
            raise RuntimeError('Cannot instrument speech request dispatch unambiguously')
        broker_text = broker_text.replace(counter_anchor,
                counter_anchor + '\n    public volatile long testRequestDispatchVisits;')
        broker_text = broker_text.replace(dispatch_anchor,
                dispatch_anchor + '\n                testRequestDispatchVisits++;')
        broker = work / 'SpeechRecognitionManagerService.java'
        broker.write_text(broker_text)
        sources = [broker, *[ROOT / path for path in (
            'tests/dex-services/com/gesher/server/SpeechRecognitionContractTest.java',
            'tests/dex-services/gesher/services/SpeechRecognizerConnectionTest.java',
            'tests/cookie-callbacks/PerfettoTrace.java')]]
        if args.unpatched_aosp_base:
            client = work / CLIENT
            client.parent.mkdir(parents=True)
            shutil.copyfile(args.unpatched_aosp_base / CLIENT, client)
            with CLIENT_PATCH.open('rb') as patch:
                subprocess.run(['patch', '--batch', '--silent', '-p1'], stdin=patch, cwd=work, check=True)
            sources.append(client)
        subprocess.run([str(args.java_home / 'bin/javac'), '-encoding', 'UTF-8',
                        '-cp', classpath, '-d', str(work), *map(str, sources)], check=True)
        cleaner = work / 'cleaner' / 'sun' / 'misc'
        cleaner.mkdir(parents=True)
        for file in (args.core_classes / 'sun' / 'misc').glob('Cleaner*.class'):
            shutil.copy2(file, cleaner / file.name)
        java = [str(args.java_home / 'bin/java'), '-XX:+UnlockDiagnosticVMOptions',
                '-XX:-BytecodeVerificationRemote', '--patch-module',
                'jdk.unsupported=' + str(work / 'cleaner'), '-cp', str(work) + os.pathsep + classpath]
        subprocess.run([*java, '-Dgesher.test.speech.dispatch-budget=true',
                        'com.gesher.server.SpeechRecognitionContractTest', str(binder)],
                       check=True, timeout=45)
        subprocess.run([*java, 'gesher.services.SpeechRecognizerConnectionTest', str(binder), str(looper)],
                       check=True, timeout=45)


if __name__ == '__main__':
    main()
