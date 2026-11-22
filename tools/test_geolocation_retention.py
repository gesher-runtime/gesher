#!/usr/bin/env python3
"""Run production GeolocationShim with real Android Handler/MessageQueue on a host JVM.

WebView shell/message delivery and LocationManager backend are explicit test seams.
Does not execute WebView2, Windows ART, the permission prompt or live location input.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--cc', default='cc')
    p.add_argument('--source-ref', help='Compile an earlier production shim as a negative control')
    a = p.parse_args()
    cp = os.pathsep.join(str(x.resolve()) for x in (a.app_classes, a.framework_classes, a.core_classes))
    javac, java = (str(a.java_home.resolve() / 'bin' / x) for x in ('javac', 'java'))
    with tempfile.TemporaryDirectory(prefix='gesher-geolocation-') as temp:
        work = Path(temp)
        classes = work / 'classes'; classes.mkdir()
        windows = os.name == 'nt'
        lib = work / ('geolocation_host.dll' if windows else 'libgeolocation_host.so')
        subprocess.run([a.cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared',
            *([] if windows else ['-fPIC', '-pthread']),
            '-I' + str(a.java_home.resolve() / 'include'),
            '-I' + str(a.java_home.resolve() / 'include' / ('win32' if windows else 'linux')),
            str(ROOT / 'tests/geolocation/framework_host.c'), '-o', str(lib),
            *(['-lwinpthread', '-static'] if windows else [])], check=True)
        path = 'app/java/com/gesher/app/webview/GeolocationShim.java'
        source = ROOT / path
        if a.source_ref:
            source = work / 'GeolocationShim.java'
            source.write_bytes(subprocess.check_output(['git', 'show', a.source_ref + ':' + path], cwd=ROOT))
        # Check the source against the real application interfaces before compiling
        # the fixture's controlled shell and location backend around it.
        production = work / 'production'; production.mkdir()
        subprocess.run([javac, '-encoding', 'UTF-8', '-cp', cp, '-d', str(production), str(source)], check=True)
        subprocess.run([javac, '-encoding', 'UTF-8', '-cp', cp, '-d', str(classes), str(source),
            *map(str, (ROOT / 'tests/geolocation').glob('*.java'))], check=True)
        subprocess.run([java, '-XX:-UsePerfData', '-XX:+UnlockDiagnosticVMOptions',
            '-XX:-BytecodeVerificationRemote', '-cp', str(classes) + os.pathsep + cp,
            'com.gesher.app.webview.GeolocationHost', str(lib)], check=True, timeout=60)


if __name__ == '__main__':
    main()
