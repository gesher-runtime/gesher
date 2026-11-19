#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Run the NSD manager and browse state machine over explicit native-boundary fixtures.

No host result is Windows DNS-SD execution. The required Windows gate exercises
DnsStartMulticastQuery with a separate raw-wire fixture publisher.
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
    args = p.parse_args()
    cp = os.pathsep.join(str(x.resolve()) for x in
                        (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix='gesher-nsd-browse-') as tmp:
        work = Path(tmp)
        records = work / 'nsd_browse_records_test'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'compat/net'),
                        str(ROOT / 'tests/native/nsd_browse_records_test.cc'), '-o', str(records)], check=True)
        subprocess.run([str(records)], check=True, timeout=30)
        library = work / 'libnsd_host.so'
        subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/component-caller/framework_host.c'), '-o', str(library)], check=True)
        sources = [ROOT / 'app/java/com/gesher/net/WindowsNsd.java',
                   ROOT / 'app/java/android/net/nsd/NsdManager.java']
        for name in ('NsdContractTest', 'NsdBrowseTransportTest'):
            sources.append(ROOT / ('tests/dex-services/gesher/services/' + name + '.java'))
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', cp, '-d', str(work), *map(str, sources)], check=True)
        for name in ('NsdContractTest', 'NsdBrowseTransportTest'):
            subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                            '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
                            '-cp', str(work) + os.pathsep + cp, 'gesher.services.' + name,
                            str(library)], check=True, timeout=60)


if __name__ == '__main__':
    main()
