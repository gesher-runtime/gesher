#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise production android.net.http over real HTTP sockets on a host JVM.

Context and Network are host fixtures. Optional HTTP/2 checks compile the pinned
AOSP framing and Okio sources plus this revision's provider patches, with platform policy/annotation/logger
fixtures. All engine/request/upload/callback code is production code.
"""
import argparse
import hashlib
import io
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def prepare_provider(checkout, destination, source_ref):
    """Reconstruct the pinned provider; never inherit another checkout's patches.

    CI's AOSP checkout is already patched by fetch-aosp.sh. Historical controls
    must still compile their own unmodified provider, not the candidate's fix.
    """
    revision = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=checkout,
                              capture_output=True, text=True, check=True).stdout.strip()
    if revision != 'e3d37d381375156dfad90887870775137e0481bf':
        raise RuntimeError('HTTP/2 contracts require the android-16.0.0_r4 OkHttp revision')
    archive = subprocess.run(['git', 'archive', 'HEAD', 'repackaged'], cwd=checkout,
                             capture_output=True, check=True).stdout
    destination.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
        for member in tree.getmembers():
            path = Path(member.name)
            if path.is_absolute() or '..' in path.parts or path.parts[0] != 'repackaged':
                raise RuntimeError('Unexpected provider archive member')
            target = destination / path
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(tree.extractfile(member).read())
            else:
                raise RuntimeError('Unexpected non-file provider archive member')
    if source_ref:
        names = subprocess.run(['git', 'ls-tree', '-r', '--name-only', source_ref,
                                '--', 'patches/external/okhttp'], cwd=ROOT,
                               capture_output=True, text=True, check=True).stdout.splitlines()
        patches = [(name, subprocess.run(['git', 'show', source_ref + ':' + name],
                    cwd=ROOT, capture_output=True, check=True).stdout)
                   for name in names if name.endswith('.patch')]
    else:
        patches = [(path.relative_to(ROOT).as_posix(), path.read_bytes())
                   for path in sorted((ROOT / 'patches/external/okhttp').glob('*.patch'))]
    print('HTTP2_PROVIDER_BASE ' + revision, flush=True)
    for name, content in patches:
        subprocess.run(['git', 'apply', '--check', '-'], input=content, cwd=destination, check=True)
        subprocess.run(['git', 'apply', '-'], input=content, cwd=destination, check=True)
        print('HTTP2_PROVIDER_PATCH ' + name + ' SHA256 ' + hashlib.sha256(content).hexdigest(), flush=True)
    return destination


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--java-home', type=Path, required=True)
    p.add_argument('--okhttp-source', type=Path,
                   help='Optional pinned AOSP external/okhttp checkout for real HTTP/2 wire tests')
    p.add_argument('--http2-mode', action='append',
                   help='Run selected HTTP/2 smoke mode(s), instead of the full mode list')
    p.add_argument('--source-ref', help='Compile the complete unchanged android.net.http package from this git ref')
    p.add_argument('--sessions-only', action='store_true',
                   help='Run the actual HTTP/2 session wire corpus only')
    p.add_argument('--transfer-only', action='store_true',
                   help='Run only the HTTP/2 transfer contracts and component measurements')
    p.add_argument('--credit-only', action='store_true',
                   help='Run bounded in-memory HTTP/2 receive-credit component contracts')
    args = p.parse_args()
    if (args.transfer_only or args.sessions_only or args.credit_only) and not args.okhttp_source:
        p.error('focused HTTP/2 checks require the pinned --okhttp-source')
    with tempfile.TemporaryDirectory(prefix='gesher-httpengine-') as temporary:
        work = Path(temporary)
        context = work / 'android/content/Context.java'
        context.parent.mkdir(parents=True)
        context.write_text('package android.content; public class Context {}')
        network = work / 'android/net/Network.java'
        network.parent.mkdir(parents=True, exist_ok=True)
        network.write_text('''package android.net;
            // Declared host network boundary: all DNS and sockets are real
            // loopback operations; counters verify which supplied owner is used.
            public class Network {
                private final int id;
                public int sendBufferBytes;
                public final java.util.concurrent.atomic.AtomicInteger sockets=new java.util.concurrent.atomic.AtomicInteger();
                public final java.util.concurrent.atomic.AtomicInteger resolutions=new java.util.concurrent.atomic.AtomicInteger();
                public Network() { this(0); }
                public Network(int id) { this.id=id; }
                public boolean equals(Object other) { return other instanceof Network && ((Network)other).id==id; }
                public int hashCode() { return id; }
                public java.net.URLConnection openConnection(java.net.URL url) throws java.io.IOException { return url.openConnection(); }
                public javax.net.SocketFactory getSocketFactory() {
                    return new javax.net.SocketFactory() {
                        public java.net.Socket createSocket() throws java.io.IOException {
                            sockets.incrementAndGet(); java.net.Socket socket=new java.net.Socket();
                            if (sendBufferBytes>0) socket.setSendBufferSize(sendBufferBytes);
                            return socket;
                        }
                        public java.net.Socket createSocket(String host,int port) throws java.io.IOException {
                            sockets.incrementAndGet(); return new java.net.Socket(host,port); }
                        public java.net.Socket createSocket(java.net.InetAddress host,int port) throws java.io.IOException {
                            sockets.incrementAndGet(); return new java.net.Socket(host,port); }
                        public java.net.Socket createSocket(String host,int port,java.net.InetAddress local,int localPort) throws java.io.IOException {
                            sockets.incrementAndGet(); return new java.net.Socket(host,port,local,localPort); }
                        public java.net.Socket createSocket(java.net.InetAddress host,int port,java.net.InetAddress local,int localPort) throws java.io.IOException {
                            sockets.incrementAndGet(); return new java.net.Socket(host,port,local,localPort); }
                    };
                }
                public java.net.InetAddress getByName(String host) throws java.net.UnknownHostException {
                    resolutions.incrementAndGet(); return java.net.InetAddress.getByName(host);
                }
            }''')
        sources = list((ROOT / 'app/java/android/net/http').glob('*.java'))
        if args.source_ref:
            # Compile the complete unchanged historical HTTP package. New helper
            # sources must not accidentally supply part of the measured engine.
            names = subprocess.run(['git', 'ls-tree', '-r', '--name-only', args.source_ref,
                                    '--', 'app/java/android/net/http'], cwd=ROOT,
                                   capture_output=True, text=True, check=True).stdout.splitlines()
            sources = []
            for name in names:
                if not name.endswith('.java'):
                    continue
                content = subprocess.run(['git', 'show', args.source_ref + ':' + name],
                                         cwd=ROOT, capture_output=True, check=True).stdout
                previous = work / 'production' / name
                previous.parent.mkdir(parents=True, exist_ok=True)
                previous.write_bytes(content)
                sources.append(previous)
        for source in sources:
            print(source.name + ' SHA256 ' + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
        subprocess.run([str(args.java_home / 'bin/javac'), '--release', '17', '-encoding', 'UTF-8',
                        '-d', str(work), str(context), str(network), *map(str, sources),
                        str(ROOT / 'tests/net/HttpEngineHostTest.java')], check=True, timeout=60)
        if not args.transfer_only and not args.sessions_only and not args.credit_only:
            subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work), 'HttpEngineHostTest'],
                           check=True, timeout=90)
            # URL permits installing the global factory only once in a VM.
            subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work), 'HttpEngineHostTest',
                            'global-factory'], check=True, timeout=30)
        if args.okhttp_source:
            provider = prepare_provider(args.okhttp_source, work / 'provider', args.source_ref)
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
            http = provider / 'repackaged/okhttp/src/main/java/com/android/okhttp'
            framing = list((http / 'internal/framed').glob('*.java'))
            framing += [http / p for p in ('HttpUrl.java', 'Protocol.java',
                                           'internal/Util.java', 'internal/NamedRunnable.java')]
            framing += list((provider / 'repackaged/okio/okio/src/main/java').rglob('*.java'))
            fingerprint = hashlib.sha256()
            for source in sorted(framing):
                fingerprint.update(source.relative_to(provider).as_posix().encode() + b'\0')
                fingerprint.update(source.read_bytes() + b'\0')
            print('HTTP2_PROVIDER_SHA256 ' + fingerprint.hexdigest(), flush=True)
            subprocess.run([str(args.java_home / 'bin/javac'), '--release', '17', '-encoding', 'UTF-8',
                            '-cp', str(work), '-d', str(work), str(wrapper), str(policy), str(annotation), str(logger),
                            *map(str, framing), str(ROOT / 'tests/net/Http2DeviceSmoke.java'),
                            str(ROOT / 'tests/net/Http2TransferHost.java'),
                            str(ROOT / 'tests/net/Http2SessionContracts.java'),
                            str(ROOT / 'tests/net/Http2SessionHost.java'),
                            str(ROOT / 'tests/net/Http2CreditContracts.java')],
                           check=True, timeout=60)
            if args.credit_only or not (args.sessions_only or args.transfer_only):
                subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work),
                                'Http2CreditContracts'], check=True, timeout=35)
            if args.credit_only:
                return
            if not args.sessions_only:
                subprocess.run([str(args.java_home / 'bin/java'), '-Xmx512m', '-cp', str(work),
                                'android.net.http.Http2TransferHost'], check=True, timeout=90)
            if args.transfer_only:
                return
            modes = args.http2_mode or (['sessions'] if args.sessions_only else ['', 'unary', 'gzip', 'gzip-headers', 'early-read', 'readonly-write', 'large', 'cancel', 'disabled', 'sessions'])
            for mode in modes:
                subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work), 'Http2DeviceSmoke',
                                *([mode] if mode else [])], check=True, timeout=35)
            if 'sessions' in modes or 'sessions-baseline' in modes:
                store = work / 'session-loopback.p12'
                subprocess.run([str(args.java_home / 'bin/keytool'), '-genkeypair', '-alias', 'loopback',
                                '-keyalg', 'RSA', '-keysize', '2048', '-validity', '2', '-dname', 'CN=localhost',
                                '-ext', 'SAN=ip:127.0.0.1,dns:localhost', '-storetype', 'PKCS12',
                                '-keystore', str(store), '-storepass', 'changeit', '-keypass', 'changeit',
                                '-noprompt'], check=True, timeout=30, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)
                subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work), 'Http2SessionHost',
                                str(store), *(['baseline'] if 'sessions-baseline' in modes else [])],
                               check=True, timeout=60)
            if args.sessions_only:
                return
            subprocess.run([sys.executable, str(ROOT / 'tools/test_http_engine_policy.py'),
                            '--java-home', str(args.java_home), '--okhttp-source', str(provider)],
                           check=True, timeout=180)


if __name__ == '__main__':
    main()
