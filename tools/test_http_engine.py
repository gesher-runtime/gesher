#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise production android.net.http over real HTTP sockets on a host JVM.

Context and Network are host fixtures. Optional HTTP/2 checks compile the pinned
AOSP framing and Okio sources unchanged, with platform policy/annotation/logger
fixtures. All engine/request/upload/callback code is production code.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--java-home', type=Path, required=True)
    p.add_argument('--okhttp-source', type=Path,
                   help='Optional pinned AOSP external/okhttp checkout for real HTTP/2 wire tests')
    p.add_argument('--http2-mode', action='append',
                   help='Run selected HTTP/2 smoke mode(s), instead of the full mode list')
    p.add_argument('--source-ref', help='Compare unchanged prior transport/stream sources')
    p.add_argument('--transfer-only', action='store_true',
                   help='Run only the HTTP/2 transfer contracts and component measurements')
    args = p.parse_args()
    if args.transfer_only and not args.okhttp_source:
        p.error('--transfer-only requires the pinned --okhttp-source')
    with tempfile.TemporaryDirectory(prefix='gesher-httpengine-') as temporary:
        work = Path(temporary)
        context = work / 'android/content/Context.java'
        context.parent.mkdir(parents=True)
        context.write_text('package android.content; public class Context {}')
        network = work / 'android/net/Network.java'
        network.parent.mkdir(parents=True, exist_ok=True)
        network.write_text('package android.net; public class Network { public java.net.URLConnection '
                           'openConnection(java.net.URL u) throws java.io.IOException { return u.openConnection(); }'
                           'public javax.net.SocketFactory getSocketFactory() { return javax.net.SocketFactory.getDefault(); }'
                           'public java.net.InetAddress getByName(String host) throws java.net.UnknownHostException {'
                           'return java.net.InetAddress.getByName(host); }}')
        sources = list((ROOT / 'app/java/android/net/http').glob('*.java'))
        for name in ('Http2Transport.java', 'JavaBidirectionalStream.java'):
            source = ROOT / 'app/java/android/net/http' / name
            content = source.read_bytes() if not args.source_ref else subprocess.run(
                    ['git', 'show', args.source_ref + ':app/java/android/net/http/' + name],
                    cwd=ROOT, capture_output=True, check=True).stdout
            print(name + ' SHA256 ' + hashlib.sha256(content).hexdigest(), flush=True)
            if args.source_ref:
                previous = work / 'production/android/net/http' / name
                previous.parent.mkdir(parents=True, exist_ok=True)
                previous.write_bytes(content)
                sources[sources.index(source)] = previous
        subprocess.run([str(args.java_home / 'bin/javac'), '--release', '17', '-encoding', 'UTF-8',
                        '-d', str(work), str(context), str(network), *map(str, sources),
                        str(ROOT / 'tests/net/HttpEngineHostTest.java')], check=True, timeout=60)
        if not args.transfer_only:
            subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work), 'HttpEngineHostTest'],
                           check=True, timeout=90)
            # URL permits installing the global factory only once in a VM.
            subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work), 'HttpEngineHostTest',
                            'global-factory'], check=True, timeout=30)
        if args.okhttp_source:
            wrapper = work / 'android/content/ContextWrapper.java'
            wrapper.write_text('package android.content; public class ContextWrapper extends Context {'
                               'public ContextWrapper(Context base) {}}')
            policy = work / 'android/security/NetworkSecurityPolicy.java'
            policy.parent.mkdir(parents=True)
            policy.write_text('package android.security; public class NetworkSecurityPolicy {'
                              'public static NetworkSecurityPolicy getInstance() { return new NetworkSecurityPolicy(); }'
                              'public boolean isCleartextTrafficPermitted(String host) { return true; }}')
            annotation = work / 'android/compat/annotation/UnsupportedAppUsage.java'
            annotation.parent.mkdir(parents=True)
            annotation.write_text('package android.compat.annotation; public @interface UnsupportedAppUsage {}')
            logger = work / 'com/android/okhttp/internal/Internal.java'
            logger.parent.mkdir(parents=True)
            logger.write_text('package com.android.okhttp.internal; public final class Internal {'
                              'public static final java.util.logging.Logger logger='
                              'java.util.logging.Logger.getLogger("OkHttp"); }')
            http = args.okhttp_source / 'repackaged/okhttp/src/main/java/com/android/okhttp'
            framing = list((http / 'internal/framed').glob('*.java'))
            framing += [http / p for p in ('HttpUrl.java', 'Protocol.java',
                                           'internal/Util.java', 'internal/NamedRunnable.java')]
            framing += list((args.okhttp_source / 'repackaged/okio/okio/src/main/java').rglob('*.java'))
            subprocess.run([str(args.java_home / 'bin/javac'), '--release', '17', '-encoding', 'UTF-8',
                            '-cp', str(work), '-d', str(work), str(wrapper), str(policy), str(annotation), str(logger),
                            *map(str, framing), str(ROOT / 'tests/net/Http2DeviceSmoke.java'),
                            str(ROOT / 'tests/net/Http2TransferHost.java')],
                           check=True, timeout=60)
            subprocess.run([str(args.java_home / 'bin/java'), '-Xmx512m', '-cp', str(work),
                            'android.net.http.Http2TransferHost'], check=True, timeout=90)
            if args.transfer_only:
                return
            for mode in args.http2_mode or ['', 'unary', 'gzip', 'gzip-headers', 'early-read', 'readonly-write', 'large', 'cancel', 'disabled']:
                subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work), 'Http2DeviceSmoke',
                                *([mode] if mode else [])], check=True, timeout=35)
            subprocess.run([sys.executable, str(ROOT / 'tools/test_http_engine_policy.py'),
                            '--java-home', str(args.java_home), '--okhttp-source', str(args.okhttp_source)],
                           check=True, timeout=180)


if __name__ == '__main__':
    main()
