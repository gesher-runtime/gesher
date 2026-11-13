#!/usr/bin/env python3
"""SDK-compiled Wi-Fi data contracts over the production Android 16 implementations.

The host uses the real framework classes and the existing native initialization
adapter. It omits native Parcel transport; the same fixture on Windows requires
real android_runtime and Parcelable round trips. --source-ref replaces WifiInfo
only, retaining the new value types to isolate the old missing query methods.
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
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--sdk-jar', type=Path, required=True)
    parser.add_argument('--framework-classes', type=Path, required=True)
    parser.add_argument('--core-classes', type=Path, required=True)
    parser.add_argument('--source-ref', help='Use an old WifiInfo source as a negative control')
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc:
        parser.error('A C compiler is required for the declared native initialization boundary')
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.framework_classes, args.core_classes))
    javac = str(args.java_home.resolve() / 'bin/javac')
    java = str(args.java_home.resolve() / 'bin/java')
    with tempfile.TemporaryDirectory(prefix='gesher-wifi-data-contracts-') as temporary:
        work = Path(temporary)
        classes, sdk_classes = work / 'classes', work / 'sdk-classes'
        classes.mkdir()
        sdk_classes.mkdir()
        library = work / 'libwifi_data_host.so'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home.resolve() / 'include'),
                        '-I' + str(args.java_home.resolve() / 'include/linux'),
                        str(ROOT / 'tests/component-caller/framework_host.c'), '-o', str(library)],
                       check=True)
        # Every test call site binds to the actual public Android SDK descriptors.
        subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', str(args.sdk_jar.resolve()), '-d', str(sdk_classes),
                        str(ROOT / 'tests/dex-services/gesher/services/WifiDataContractTest.java')],
                       check=True)
        sources = []
        for name in ('wifi/WifiInfo', 'wifi/WifiSsid', 'wifi/SupplicantState',
                     'wifi/util/HexEncoding', 'NetworkInfo', 'TransportInfo'):
            relative = 'app/java/android/net/' + name + '.java'
            source = ROOT / relative
            if args.source_ref and name == 'wifi/WifiInfo':
                old = subprocess.run(['git', 'show', args.source_ref + ':' + relative],
                                     cwd=ROOT, capture_output=True, check=True)
                source = work / 'WifiInfo.java'
                source.write_bytes(old.stdout)
            sources.append(str(source))
        subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', classpath, '-d', str(classes), *sources], check=True)
        subprocess.run([java, '-XX:-UsePerfData', '-XX:+UnlockDiagnosticVMOptions',
                        '-XX:-BytecodeVerificationRemote', '-cp',
                        os.pathsep.join((str(classes), str(sdk_classes), classpath)),
                        'gesher.services.WifiDataContractTest', str(library)],
                       check=True, timeout=60)
        print('Public SDK call sites and real production Wi-Fi value types; '
              'host initialization adapter only. Native Parcel is a separate Windows gate.')


if __name__ == '__main__':
    main()
