#!/usr/bin/env python3
"""SDK-compiled NDEF data contracts over the production Android 16 implementations.

The host run uses real framework Uri/Intent classes and the existing native
initialization adapter. It deliberately omits Parcel transport; the same fixture's
default Windows main requires real android_runtime and verifies Parcelable data.
--source-ref replaces only NdefMessage/NdefRecord with older production sources,
leaving the exception type available so the old data-loss behavior can be observed.
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
    parser.add_argument('--source-ref', help='Use old data classes as a negative control')
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc:
        parser.error('A C compiler is required for the declared native initialization boundary')
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.framework_classes, args.core_classes))
    javac = str(args.java_home.resolve() / 'bin/javac')
    java = str(args.java_home.resolve() / 'bin/java')
    with tempfile.TemporaryDirectory(prefix='gesher-ndef-contracts-') as temporary:
        work = Path(temporary)
        classes, sdk_classes = work / 'classes', work / 'sdk-classes'
        classes.mkdir()
        sdk_classes.mkdir()
        library = work / 'libndef_host.so'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home.resolve() / 'include'),
                        '-I' + str(args.java_home.resolve() / 'include/linux'),
                        str(ROOT / 'tests/component-caller/framework_host.c'), '-o', str(library)],
                       check=True)
        # Every test call site binds to the actual public Android SDK descriptors.
        subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', str(args.sdk_jar.resolve()), '-d', str(sdk_classes),
                        str(ROOT / 'tests/dex-services/gesher/services/NdefDataContractTest.java')],
                       check=True)
        sources = []
        for name in ('NdefMessage', 'NdefRecord', 'FormatException', 'NfcAdapter', 'NfcEvent',
                     'Tag', 'cardemulation/CardEmulation'):
            relative = 'app/java/android/nfc/' + name + '.java'
            source = ROOT / relative
            if args.source_ref and name in ('NdefMessage', 'NdefRecord'):
                old = subprocess.run(['git', 'show', args.source_ref + ':' + relative],
                                     cwd=ROOT, capture_output=True, check=True)
                source = work / (name + '.java')
                source.write_bytes(old.stdout)
            sources.append(str(source))
        subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', classpath, '-d', str(classes), *sources], check=True)
        subprocess.run([java, '-XX:-UsePerfData', '-XX:+UnlockDiagnosticVMOptions',
                        '-XX:-BytecodeVerificationRemote', '-cp',
                        os.pathsep.join((str(classes), str(sdk_classes), classpath)),
                        'gesher.services.NdefDataContractTest', str(library)],
                       check=True, timeout=60)
        print('Public SDK call sites and real production NDEF/Uri/Intent classes; '
              'host initialization adapter only. Native Parcel is a separate Windows gate.')


if __name__ == '__main__':
    main()
