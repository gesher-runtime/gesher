#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run the complete production DnsResolver and pinned AOSP CancellationSignal.

Only the synchronous Windows lookup, platform value types, and unused cancellation
Binder transport are host adapters. Admission, lookup/completion executors, copies,
deadlines, callback ownership, parsing and cancellation are production code.
The existing FrameworkFixesTest additionally exercises real Windows DNS through ART.
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    'android/net/Network.java': '''package android.net; public final class Network {
        public final int netId; public Network(int id) { netId = id; } }''',
    'android/system/ErrnoException.java': '''package android.system;
        public class ErrnoException extends Exception {
        public final int errno; public final String functionName;
        public ErrnoException(String operation, int value) {
            super(operation + ": " + value); functionName = operation; errno = value; } }''',
    'android/system/OsConstants.java': '''package android.system; public final class OsConstants {
        public static final int EINTR=4, EAGAIN=11, EMSGSIZE=90, ENOBUFS=105, ETIMEDOUT=110; }''',
    'android/compat/annotation/UnsupportedAppUsage.java': '''package android.compat.annotation;
        public @interface UnsupportedAppUsage { int maxTargetSdk() default 0; }''',
    'android/ravenwood/annotation/RavenwoodKeepWholeClass.java': '''package android.ravenwood.annotation;
        public @interface RavenwoodKeepWholeClass {}''',
    'android/os/Build.java': '''package android.os; public final class Build {
        public static final class VERSION_CODES { public static final int P = 28; } }''',
    'android/os/RemoteException.java': '''package android.os;
        public class RemoteException extends Exception {}''',
    'android/os/OperationCanceledException.java': '''package android.os;
        public class OperationCanceledException extends RuntimeException {}''',
    'android/os/ICancellationSignal.java': '''package android.os; public interface ICancellationSignal {
        void cancel() throws RemoteException;
        abstract class Stub implements ICancellationSignal {} }''',
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref', help='Prior complete resolver for public-API negative controls')
    parser.add_argument('--case', choices=('all', 'deadline', 'delayed-deadline',
                                          'recovery', 'burst', 'copy'), default='all')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-dns-admission-') as temporary:
        work = Path(temporary)
        sources = list((ROOT / 'tests/dns-resolver').glob('*.java'))
        relative = 'app/java/android/net/DnsResolver.java'
        source = subprocess.check_output(['git', 'show', args.source_ref + ':' + relative],
                                        cwd=ROOT) if args.source_ref else (ROOT / relative).read_bytes()
        target = work / 'DnsResolver.java'
        target.write_bytes(source)
        sources.append(target)
        print('Complete DnsResolver SHA256 ' + hashlib.sha256(source).hexdigest(), flush=True)
        cancellation = ROOT / 'tests/dns-resolver/CancellationSignal.java'
        print('Pinned CancellationSignal SHA256 ' +
              hashlib.sha256(cancellation.read_bytes()).hexdigest(), flush=True)
        for name, text in STUBS.items():
            target = work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding='utf-8')
            sources.append(target)
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-d', str(work), *map(str, sources)], check=True, timeout=60)
        cases = ['all', 'deadline', 'delayed-deadline'] if args.case == 'all' and not args.source_ref else [args.case]
        if args.source_ref and args.case == 'all':
            cases = ['burst']
        for case in cases:
            subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx256m',
                            '-cp', str(work), 'android.net.DnsResolverHost', case],
                           check=True, timeout=75)


if __name__ == '__main__':
    main()
