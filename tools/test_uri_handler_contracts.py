#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Host contracts of the production WindowsUriHandlers (mailto/tel/sms/geo intents
resolved to Windows' registered protocol handlers).

Compiles app/java/com/gesher/server/WindowsUriHandlers.java unchanged with
`javac -Xlint:all,-classfile -Werror` against AOSP's own android-16 classes
(Robolectric android-all, tools/android_all.py) and runs
tests/uri-handlers/UriHandlerContracts.java.in on JDK 21. Also exercises unchanged
production DialRequests sequencing against an injected Windows UI/shell boundary.
Host-only stand-ins cover native SystemClock, WindowsHost and server logging; real
Intent, IntentFilter and Uri classes come from the verified android-all jar.

--source-ref REF compiles the class from REF instead: on upstream/main, which
has no Windows URI handlers, it must fail (negative control).
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import android_all  # noqa: E402

PRODUCTION = 'app/java/com/gesher/server/WindowsUriHandlers.java'
DIAL_PRODUCTION = 'app/java/com/gesher/server/DialRequests.java'
CONTRACTS = ROOT / 'tests/uri-handlers/UriHandlerContracts.java.in'
DIAL_CONTRACTS = ROOT / 'tests/uri-handlers/DialRequestContracts.java.in'
# Host stand-ins of android-all classes whose methods are native (boundary only).
STANDINS = ROOT / 'tests/uri-handlers/standins'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--java-home', type=Path, default=os.environ.get('JAVA_HOME'))
    parser.add_argument('--android-all', type=Path, help='a local android-all jar (verified)')
    parser.add_argument('--source-ref', help='git ref to take the production class from')
    args = parser.parse_args()
    bin_dir = (args.java_home / 'bin') if args.java_home else None
    javac = str(bin_dir / 'javac') if bin_dir else 'javac'
    java = str(bin_dir / 'java') if bin_dir else 'java'
    jar = android_all.jar(args.android_all)
    with tempfile.TemporaryDirectory(prefix='gesher-uri-handlers-') as temporary:
        work = Path(temporary)
        src = work / 'src/com/gesher/server'
        src.mkdir(parents=True)
        if args.source_ref:
            shown = subprocess.run(['git', 'show', f'{args.source_ref}:{PRODUCTION}'], cwd=ROOT,
                                   capture_output=True)
            if shown.returncode != 0:
                print(f'FAIL: {PRODUCTION} does not exist at {args.source_ref}', flush=True)
                return 1
            production = shown.stdout
        else:
            production = (ROOT / PRODUCTION).read_bytes()
        print(f'{PRODUCTION} SHA256 {hashlib.sha256(production).hexdigest()}', flush=True)
        (src / 'WindowsUriHandlers.java').write_bytes(production)
        (src / 'UriHandlerContracts.java').write_bytes(CONTRACTS.read_bytes())
        dial_source = (subprocess.check_output(['git', 'show', f'{args.source_ref}:{DIAL_PRODUCTION}'], cwd=ROOT)
                       if args.source_ref else (ROOT / DIAL_PRODUCTION).read_bytes())
        print(f'{DIAL_PRODUCTION} SHA256 {hashlib.sha256(dial_source).hexdigest()}', flush=True)
        (src / 'DialRequests.java').write_bytes(dial_source)
        (src / 'DialRequestContracts.java').write_bytes(DIAL_CONTRACTS.read_bytes())
        standins = []
        for stand_in in sorted(STANDINS.rglob('*.java.in')):
            target = work / 'src' / stand_in.relative_to(STANDINS).with_suffix('')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(stand_in.read_bytes())
            standins.append(str(target))
        classes = work / 'classes'
        compiled = subprocess.run([javac, '-Xlint:all,-classfile', '-Werror', '-encoding', 'UTF-8',
                                   '-cp', str(jar), '-d', str(classes),
                                   str(src / 'WindowsUriHandlers.java'),
                                   str(src / 'UriHandlerContracts.java'),
                                   str(src / 'DialRequests.java'),
                                   str(src / 'DialRequestContracts.java'), *standins])
        if compiled.returncode != 0:
            print('FAIL: compilation', flush=True)
            return 1
        ran = subprocess.run([java, '-Xmx256m', '-cp', os.pathsep.join([str(classes), str(jar)]),
                              'com.gesher.server.UriHandlerContracts'])
        if ran.returncode != 0:
            print('FAIL: contracts', flush=True)
            return 1
        dial = subprocess.run([java, '-Xmx256m', '-cp', os.pathsep.join([str(classes), str(jar)]),
                               'com.gesher.server.DialRequestContracts'])
        if dial.returncode != 0:
            print('FAIL: dial request contracts', flush=True)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
