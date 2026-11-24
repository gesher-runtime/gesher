#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Host contracts of the production BackgroundRestriction (the network snapshot's
"background" line and ConnectivityManager.getRestrictBackgroundStatus's answer).

Compiles app/java/com/gesher/net/BackgroundRestriction.java unchanged with
`javac -Xlint:all,-classfile -Werror` against AOSP's own android-16 classes
(ConnectivityManager's constants; Robolectric android-all, tools/android_all.py)
and runs tests/metered-network/MeteredNetworkContracts.java.in on JDK 21.
WindowsNetworks itself uses libcore-only methods and is exercised on ART
(gesher.services.RestrictBackgroundTest) instead.

--source-ref REF compiles the class from REF instead: on upstream/main, which
reads no background restriction, it must fail (negative control).
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

PRODUCTION = ['app/java/com/gesher/net/BackgroundRestriction.java']
CONTRACTS = ROOT / 'tests/metered-network/MeteredNetworkContracts.java.in'


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
    with tempfile.TemporaryDirectory(prefix='gesher-metered-network-') as temporary:
        work = Path(temporary)
        src = work / 'src/com/gesher/net'
        src.mkdir(parents=True)
        sources = []
        for relative in PRODUCTION:
            if args.source_ref:
                shown = subprocess.run(['git', 'show', f'{args.source_ref}:{relative}'], cwd=ROOT,
                                       capture_output=True)
                if shown.returncode != 0:
                    print(f'FAIL: {relative} does not exist at {args.source_ref}', flush=True)
                    return 1
                production = shown.stdout
            else:
                production = (ROOT / relative).read_bytes()
            print(f'{relative} SHA256 {hashlib.sha256(production).hexdigest()}', flush=True)
            target = src / Path(relative).name
            target.write_bytes(production)
            sources.append(str(target))
        (src / 'MeteredNetworkContracts.java').write_bytes(CONTRACTS.read_bytes())
        classes = work / 'classes'
        compiled = subprocess.run([javac, '-Xlint:all,-classfile', '-Werror', '-encoding', 'UTF-8',
                                   '-cp', str(jar), '-d', str(classes),
                                   *sources, str(src / 'MeteredNetworkContracts.java')])
        if compiled.returncode != 0:
            print('FAIL: compilation', flush=True)
            return 1
        ran = subprocess.run([java, '-Xmx256m', '-cp', os.pathsep.join([str(classes), str(jar)]),
                              'com.gesher.net.MeteredNetworkContracts'])
        if ran.returncode != 0:
            print('FAIL: contracts', flush=True)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
