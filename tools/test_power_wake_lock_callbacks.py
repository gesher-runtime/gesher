#!/usr/bin/env python3
"""Execute real power-service callback/state logic with explicit host native/token boundaries."""
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
    parser.add_argument('--source-ref', help='Compile the older production service as a negative control')
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc: parser.error('A C compiler is required')
    classpath = os.pathsep.join(str(p.resolve()) for p in (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix='gesher-power-callbacks-') as temporary:
        work = Path(temporary); classes = work / 'classes'; classes.mkdir()
        library = work / 'libpower_host.so'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC', '-pthread',
                        '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/power/framework_host.c'), '-o', str(library)], check=True)
        source = ROOT / 'app/java/com/gesher/server/PowerManagerService.java'
        if args.source_ref:
            before = subprocess.run(['git', 'show', args.source_ref + ':app/java/com/gesher/server/PowerManagerService.java'],
                                    cwd=ROOT, capture_output=True, check=True)
            source = work / 'PowerManagerService.java'; source.write_bytes(before.stdout)
        subprocess.run([str(args.java_home / 'bin/javac'), '-encoding', 'UTF-8', '-cp', classpath, '-d', str(classes),
                        str(source), str(ROOT / 'tests/dex-services/gesher/services/PowerWakeLockCallbackTest.java'),
                        str(ROOT / 'tests/power/PowerWakeLockHost.java')], check=True)
        environment = os.environ.copy(); environment.pop('GESHER_BINDER_UID', None)
        subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(classes) + os.pathsep + classpath,
                        'PowerWakeLockHost', str(library)], env=environment, check=True, timeout=60)

if __name__ == '__main__': main()
