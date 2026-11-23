#!/usr/bin/env python3
"""Run the production telephony services against synthetic modems and callers.

WindowsCellular.Source (raw modem snapshots) and TelephonyService.Callers (Binder
identity, granted permissions, app ops, location switch) are the explicit fixture
boundaries; synthetic modems exist only in tests/telephony. The parser, translations,
AOSP permission rules, subscriptions and listener delivery are the production Java,
compiled fresh from app/java. This is a host JVM run: it does not exercise ART, Binder
transactions, Windows' Mobile Broadband API or a physical modem.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--cc', default='cc', help='C compiler for the host natives (default: cc)')
    args = parser.parse_args()
    if not shutil.which(args.cc):
        parser.error('A C compiler is required for the host native boundary')
    cp = os.pathsep.join(str(path.resolve()) for path in
                         (args.app_classes, args.framework_classes, args.core_classes))
    windows = (args.java_home / 'include/win32').is_dir()
    with tempfile.TemporaryDirectory(prefix='telephony-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'
        classes.mkdir()
        library = work / ('telephony_host.dll' if windows else 'libtelephony_host.so')
        subprocess.run([args.cc, '-std=c11', '-shared', *([] if windows else ['-fPIC']),
                        '-I' + str(args.java_home / 'include'),
                        '-I' + str(args.java_home / 'include' / ('win32' if windows else 'linux')),
                        str(ROOT / 'tests/telephony/framework_host.c'), '-o', str(library)], check=True)
        sources = [ROOT / 'app/java/com/gesher/server/WindowsCellular.java',
                   ROOT / 'app/java/com/gesher/server/TelephonyService.java',
                   ROOT / 'tests/telephony/TelephonyHost.java']
        subprocess.run([str(args.java_home / 'bin/javac'), '-encoding', 'UTF-8', '-nowarn', '-cp', cp,
                        '-d', str(classes), *map(str, sources)], check=True)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                        '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
                        '-Djava.library.path=' + str(work),
                        '-cp', str(classes) + os.pathsep + cp, 'com.gesher.server.TelephonyHost',
                        str(library), str(classes)],
                       check=True, timeout=60)


if __name__ == '__main__':
    sys.exit(main())
