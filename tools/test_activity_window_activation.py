#!/usr/bin/env python3
"""Run the complete production resume callback and window activation on a host JVM.

Activity/window records are admitted by the fixture, the Handler queue is drained
explicitly, and JNI show/activate calls are observed. Production selection,
policy, lifetime checks, Application dispatch and NativeWindow wrappers are unchanged.
This does not execute Win32 or Android lifecycle transactions. --source-ref uses
the same assertions against older production sources as a negative control.
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
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--source-ref')
    parser.add_argument('--case', default='all')
    args = parser.parse_args()
    compiler = shutil.which('cc')
    if not compiler:
        parser.error('A C compiler is required for the host JNI boundaries')
    classpath = os.pathsep.join(str(p.resolve()) for p in
                               (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix='gesher-window-activation-') as directory:
        work = Path(directory)
        classes = work / 'classes'
        classes.mkdir()
        library = work / 'libactivity_window_host.so'
        subprocess.run([compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home / 'include'),
                        '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/window-activation/framework_host.c'),
                        '-o', str(library)], check=True, timeout=60)
        sources = []
        for name in ('WindowSession', 'ActivityClientHook', 'NativeWindow'):
            relative = 'app/java/com/gesher/app/window/' + name + '.java'
            source = ROOT / relative
            if args.source_ref:
                source = work / (name + '.java')
                source.write_bytes(subprocess.check_output(
                    ['git', 'show', args.source_ref + ':' + relative], cwd=ROOT))
            sources.append(str(source))
        sources.append(str(ROOT / 'tests/window-activation/ActivityWindowActivationHost.java'))
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData',
                        '-encoding', 'UTF-8', '-cp', classpath, '-d', str(classes),
                        *sources], check=True, timeout=90)
        cleaner = work / 'cleaner/sun/misc'
        cleaner.mkdir(parents=True)
        inputs = list((args.core_classes / 'sun/misc').glob('Cleaner*.class'))
        if not inputs:
            raise ValueError('Android core classes must include their Cleaner implementation')
        for source in inputs:
            shutil.copy2(source, cleaner / source.name)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                        '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
                        '--patch-module', 'jdk.unsupported=' + str(work / 'cleaner'),
                        '-cp', str(classes) + os.pathsep + classpath,
                        'ActivityWindowActivationHost', str(library), args.case],
                       check=True, timeout=45)


if __name__ == '__main__':
    main()
