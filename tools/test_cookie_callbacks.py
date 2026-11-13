#!/usr/bin/env python3
"""Public SDK cookie call sites and real production manager/Android Handler/Looper.

Only native queue waits, identity/diagnostics and WebView2 completion are adapted
for a host JVM. The same SDK fixture is a separate real Windows/WebView2 gate.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes', 'sdk-jar'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--source-ref', help='Compile an earlier production manager as a negative control')
    p.add_argument('--case', choices=('async', 'immediate', 'unavailable', 'all'), default='all')
    a = p.parse_args()
    cp = os.pathsep.join(str(x.resolve()) for x in
                        (a.app_classes, a.framework_classes, a.core_classes))
    javac, java = (str(a.java_home.resolve() / 'bin' / x) for x in ('javac', 'java'))
    with tempfile.TemporaryDirectory(prefix='gesher-cookie-callbacks-') as temp:
        work = Path(temp)
        classes = work / 'classes'; classes.mkdir()
        sdk = work / 'sdk'; sdk.mkdir()
        lib = work / 'libcookie_host.so'
        subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC', '-pthread',
            '-I' + str(a.java_home.resolve() / 'include'),
            '-I' + str(a.java_home.resolve() / 'include/linux'),
            str(ROOT / 'tests/cookie-callbacks/framework_host.c'), '-o', str(lib)], check=True)
        subprocess.run([javac, '-encoding', 'UTF-8', '-cp', str(a.sdk_jar.resolve()), '-d', str(sdk),
            str(ROOT / 'tests/dex-services/gesher/services/CookieCallbackContractTest.java')], check=True)
        path = 'app/java/com/gesher/app/webview/GesherCookieManager.java'
        source = ROOT / path
        if a.source_ref:
            source = work / 'GesherCookieManager.java'
            source.write_bytes(subprocess.check_output(['git', 'show', a.source_ref + ':' + path], cwd=ROOT))
        subprocess.run([javac, '-encoding', 'UTF-8', '-cp', str(sdk) + os.pathsep + cp,
            '-d', str(classes), str(source),
            *map(str, (ROOT / 'tests/cookie-callbacks').glob('*.java'))], check=True)
        for case in (('async', 'immediate', 'unavailable') if a.case == 'all' else (a.case,)):
            subprocess.run([java, '-XX:-UsePerfData', '-XX:+UnlockDiagnosticVMOptions',
                '-XX:-BytecodeVerificationRemote', '-cp',
                os.pathsep.join((str(classes), str(sdk), cp)), 'CookieCallbackHost', str(lib), case],
                check=True, timeout=75)


if __name__ == '__main__':
    main()
