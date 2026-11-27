#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run the production DownloadManager provider, queue and transfer on a host JVM.

DownloadRequests, DownloadQueue, DownloadTransfer, DeviceConstraints and the internal
SharedStorageOwnership check are compiled unchanged. AOSP's
SQLiteQueryBuilder and SQLiteTokenizer (android-16.0.0_r4, pinned by SHA-256) provide
the strict query building the provider relies on; SQL runs in a real SQLite engine
(sqlite-jdbc, pinned by SHA-256) behind a thin SQLiteDatabase stand-in. HTTP goes over
real sockets to a local server (redirects, failures, cut connections, Range/If-Match
resumption). The android.* constant classes and the Gesher Host/Environment are
boundary stand-ins; DownloadProvider's Binder, permission and Windows glue is not
exercised here.

Use --source-ref REF to show the state before this change: without the production
classes there is no content://downloads provider and the run fails.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PRODUCTION = ('DownloadTransfer', 'DownloadRequests', 'DownloadQueue', 'DeviceConstraints',
              'SharedStorageOwnership')
INPUTS = {
    'sqlite-jdbc-3.46.1.3.jar': (
        'https://repo.maven.apache.org/maven2/org/xerial/sqlite-jdbc/3.46.1.3/sqlite-jdbc-3.46.1.3.jar',
        '4a4832720a65eaf7f4d6fd7ede52087b994dc5633c076f9e994dc0c8b4b0b4fa'),
    'SQLiteQueryBuilder.java': (
        'https://raw.githubusercontent.com/msft-mirror-aosp/platform.frameworks.base/'
        'android-16.0.0_r4/core/java/android/database/sqlite/SQLiteQueryBuilder.java',
        'e3b1ef3f42cdc59f1b5b1f5beb1b005a131c156ddcdcb4514f54d386777190b6'),
    'SQLiteTokenizer.java': (
        'https://raw.githubusercontent.com/msft-mirror-aosp/platform.frameworks.base/'
        'android-16.0.0_r4/core/java/android/database/sqlite/SQLiteTokenizer.java',
        'a64f3c456e929df14efcb207c79ca1fe0647683da738965556ffc09e61ea9d8b'),
}


def fetch(cache: Path, name: str) -> Path:
    url, digest = INPUTS[name]
    target = cache / name
    if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == digest:
        return target
    cache.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url, timeout=120) as response:
        data = response.read()
    actual = hashlib.sha256(data).hexdigest()
    if actual != digest:
        raise RuntimeError(f'{name}: SHA-256 {actual} does not match the pinned {digest}')
    partial = target.with_suffix(target.suffix + '.partial')
    partial.write_bytes(data)
    partial.replace(target)
    return target


def production_source(ref: str | None, name: str) -> str | None:
    relative = f'app/java/com/gesher/server/{name}.java'
    if not ref:
        return (ROOT / relative).read_text()
    shown = subprocess.run(['git', 'show', f'{ref}:{relative}'], cwd=ROOT,
                           capture_output=True, text=True)
    return shown.stdout if shown.returncode == 0 else None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--java-home', default=os.environ.get('JAVA_HOME'))
    parser.add_argument('--source-ref', help='compile production classes from this Git revision')
    parser.add_argument('--cache-dir', type=Path,
                        default=Path(os.environ.get('RUNNER_TEMP', tempfile.gettempdir()))
                        / 'gesher-download-contract-inputs')
    args = parser.parse_args()
    executable = lambda name: str(Path(args.java_home) / 'bin' / name) if args.java_home else name

    sources = {}
    for name in PRODUCTION:
        source = production_source(args.source_ref, name)
        if source is None and name == 'DeviceConstraints':
            # Before the queue read charging and idle state; its contracts show the effect.
            print(f'{name}.java does not exist at {args.source_ref}', flush=True)
            continue
        if source is None:
            if name == 'SharedStorageOwnership':
                print(f'FAIL {name}.java does not exist at {args.source_ref}: '
                      'no internal MediaStore ownership check', flush=True)
                sys.exit(1)
            print(f'FAIL {name}.java does not exist at {args.source_ref}: no content://downloads '
                  'provider, so DownloadManager.enqueue fails with IllegalArgumentException '
                  '"Unknown URL content://downloads/my_downloads"', flush=True)
            sys.exit(1)
        print(f'{name} SHA256 {hashlib.sha256(source.encode()).hexdigest()}', flush=True)
        sources[name] = source

    jar = fetch(args.cache_dir, 'sqlite-jdbc-3.46.1.3.jar')
    aosp = [fetch(args.cache_dir, name) for name in ('SQLiteQueryBuilder.java', 'SQLiteTokenizer.java')]
    with tempfile.TemporaryDirectory(prefix='gesher-downloads-') as temporary:
        work = Path(temporary)
        server = work / 'src/com/gesher/server'
        sqlite = work / 'src/android/database/sqlite'
        server.mkdir(parents=True)
        sqlite.mkdir(parents=True)
        for name, source in sources.items():
            (server / f'{name}.java').write_text(source)
        for path in aosp:
            shutil.copy2(path, sqlite / path.name)
        files = [str(p) for p in (work / 'src').rglob('*.java')]
        files += [str(p) for p in (ROOT / 'tests/downloads/standins').rglob('*.java')]
        files.append(str(ROOT / 'tests/downloads/DownloadManagerHost.java'))
        files.append(str(ROOT / 'tests/storage-ownership/SharedStorageOwnershipTest.java'))
        classes = work / 'classes'
        subprocess.run([executable('javac'), '-nowarn', '-encoding', 'UTF-8', '-proc:none',
                        '-d', str(classes), '-cp', str(jar), *files], check=True, timeout=180)
        subprocess.run([executable('java'), '-cp', os.pathsep.join((str(classes), str(jar))),
                        'com.gesher.server.DownloadManagerHost'], check=True, timeout=180)
        subprocess.run([executable('java'), '-cp', str(classes),
                        'com.gesher.server.SharedStorageOwnershipTest'], check=True, timeout=30)


if __name__ == '__main__':
    main()
