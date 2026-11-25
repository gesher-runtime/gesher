#!/usr/bin/env python3
"""Exercise production floating-activity parent selection and task placement.

The host JVM runs the real WindowSession, NativeWindow wrappers and Android
Activity/theme classes. The fixture admits Activity/window records and observes
native geometry calls; it does not create Win32 windows or run app lifecycles.
--source-ref runs the same assertions against an earlier production revision.
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
    with tempfile.TemporaryDirectory(prefix='gesher-window-placement-') as directory:
        work = Path(directory)
        classes = work / 'classes'
        classes.mkdir()
        library = work / 'libwindow_placement_host.so'
        subprocess.run([compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home / 'include'),
                        '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/window-placement/framework_host.c'),
                        '-o', str(library)], check=True, timeout=60)
        sources = []
        for name in ('WindowSession', 'NativeWindow'):
            relative = 'app/java/com/gesher/app/window/' + name + '.java'
            source = ROOT / relative
            if args.source_ref:
                source = work / (name + '.java')
                source.write_bytes(subprocess.check_output(
                    ['git', 'show', args.source_ref + ':' + relative], cwd=ROOT))
            sources.append(str(source))
        sources.append(str(ROOT / 'tests/window-placement/WindowParentPlacementHost.java'))
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
                        'WindowParentPlacementHost', str(library), args.case],
                       check=True, timeout=45)


if __name__ == '__main__':
    main()
