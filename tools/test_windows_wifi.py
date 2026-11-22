#!/usr/bin/env python3
"""Run actual WindowsWifi Java cache/scan logic with a declared native WLAN boundary.

The companion native test compiles the production Windows adapter against a Win32
call boundary. Neither host test claims physical radio, Windows ACL, or ART validation.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    cp = os.pathsep.join(str(path.resolve()) for path in
                        (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix='windows-wifi-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'
        classes.mkdir()
        library = work / 'libwifi_host.so'
        subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/windows-wifi/framework_host.c'), '-o', str(library)], check=True)
        sources = [ROOT / 'app/java/com/gesher/net/WindowsWifi.java',
                   ROOT / 'tests/dex-services/com/gesher/net/WindowsWifiFixture.java',
                   ROOT / 'tests/windows-wifi/WindowsWifiHost.java']
        subprocess.run([str(args.java_home / 'bin/javac'), '-encoding', 'UTF-8', '-cp', cp,
                        '-d', str(classes), *map(str, sources)], check=True)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                        '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
                        '-cp', str(classes) + os.pathsep + cp, 'WindowsWifiHost', str(library), str(classes)],
                       check=True, timeout=45)

if __name__ == '__main__':
    main()
