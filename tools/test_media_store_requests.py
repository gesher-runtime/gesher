#!/usr/bin/env python3
"""SDK 36 call sites against production MediaStore and real framework resolver contracts.

The provider boundary is controlled by the Java fixture. Native initialization,
properties and process identity use the existing host adapter; no native Parcel,
remote Binder, media database, permission UI or Windows behavior is claimed here.
The public dex main uses the real android_runtime library on Windows.

Use --source-ref with an older revision as a negative control. Test code is still
compiled against the current facade, then the older classes take precedence at
runtime so absent descriptors fail at the SDK-compiled invoke instruction.
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
    parser.add_argument('--app-classes', type=Path, required=True)
    parser.add_argument('--source-ref', help='Run with older production facade classes')
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc:
        parser.error('A C compiler is required for the declared native boundaries')
    javac = str(args.java_home.resolve() / 'bin/javac')
    java = str(args.java_home.resolve() / 'bin/java')
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes))
    relative_sources = ('app/java/android/provider/MediaStore.java',
                        'app/java/android/net/ConnectivityManager.java')
    with tempfile.TemporaryDirectory(prefix='gesher-media-store-requests-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'
        sdk_classes = work / 'sdk-classes'
        classes.mkdir()
        sdk_classes.mkdir()
        library = work / 'libmedia_store_host.so'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home.resolve() / 'include'),
                        '-I' + str(args.java_home.resolve() / 'include/linux'),
                        str(ROOT / 'tests/component-caller/framework_host.c'), '-o', str(library)],
                       check=True)
        # This compilation intentionally has only the public SDK on its classpath.
        subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', str(args.sdk_jar.resolve()), '-d', str(sdk_classes),
                        str(ROOT / 'tests/dex-services/gesher/services/MediaStoreRequestSdkCalls.java')],
                       check=True)
        sources = [str(ROOT / relative) for relative in relative_sources]
        sources.append(str(ROOT / 'tests/dex-services/gesher/services/MediaStoreRequestContractTest.java'))
        subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', str(sdk_classes) + os.pathsep + classpath,
                        '-d', str(classes), *sources], check=True)
        runtime = [str(classes), str(sdk_classes), classpath]
        if args.source_ref:
            older = work / 'older'
            older.mkdir()
            old_sources = []
            for relative in relative_sources:
                data = subprocess.run(['git', 'show', args.source_ref + ':' + relative],
                                      cwd=ROOT, capture_output=True, check=True).stdout
                target = older / Path(relative).name
                target.write_bytes(data)
                old_sources.append(str(target))
            subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                            '-cp', classpath, '-d', str(older), *old_sources], check=True)
            runtime.insert(0, str(older))
        subprocess.run([java, '-XX:-UsePerfData', '-XX:+UnlockDiagnosticVMOptions',
                        '-XX:-BytecodeVerificationRemote', '-cp', os.pathsep.join(runtime),
                        'gesher.services.MediaStoreRequestContractTest', str(library)],
                       check=True, timeout=90)
        print('SDK-compiled calls, production facades and real ContentResolver/Bundle/ClipData; '
              'controlled provider and host native initialization boundaries.')


if __name__ == '__main__':
    main()
