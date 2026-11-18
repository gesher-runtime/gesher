#!/usr/bin/env python3
"""Compile full production UiaBridge; test weak cache ownership on a host JVM.

Window discovery/owner construction is a fixture boundary. Actual UIA, Android
attachment and native request cancellation remain Windows/ART validation.
--source-ref runs the same retention assertion on older production source.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cc', default=shutil.which('cc'))
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    if not args.cc:
        parser.error('A GNU-compatible C compiler is required')
    with tempfile.TemporaryDirectory(prefix='gesher-uia-cache-') as directory:
        work = Path(directory)
        windows = os.name == 'nt'
        lib = work / ('uia_host.dll' if windows else 'libuia_host.so')
        subprocess.run([args.cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared',
                        *([] if windows else ['-fPIC']),
                        '-I' + str(args.java_home / 'include'),
                        '-I' + str(args.java_home / 'include' / ('win32' if windows else 'linux')),
                        str(ROOT / 'tests/uia-cache/framework_host.c'), '-o', str(lib),
                        *(['-lwinpthread', '-static'] if windows else [])], check=True, timeout=60)
        relative = 'app/java/com/gesher/app/accessibility/UiaBridge.java'
        source = ROOT / relative
        if args.source_ref:
            source = work / 'UiaBridge.java'
            source.write_bytes(subprocess.check_output(['git', 'show', args.source_ref + ':' + relative], cwd=ROOT))
        classpath = os.pathsep.join(str(p.resolve()) for p in (args.app_classes, args.framework_classes, args.core_classes))
        classes = work / 'classes'
        classes.mkdir()
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', classpath, '-d', str(classes), str(source),
                        str(ROOT / 'tests/uia-cache/UiaCacheHost.java')], check=True, timeout=60)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                        '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
                        '-cp', str(classes) + os.pathsep + classpath,
                        'UiaCacheHost', str(lib)], check=True, timeout=30)


if __name__ == '__main__':
    main()
