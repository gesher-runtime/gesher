#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run the production MediaStore index on a host JVM against real SQLite and a real volume.

MediaStoreIndex is compiled unchanged. AOSP's SQLiteQueryBuilder and SQLiteTokenizer
(android-16.0.0_r4, pinned by SHA-256) provide the strict query building the provider
relies on; SQL runs in a real SQLite engine (sqlite-jdbc, pinned by SHA-256) behind a
thin SQLiteDatabase stand-in, over a database file that is closed and reopened to model
a system-server restart. Files are real files in a temporary shared-volume directory.
The android.* value classes and the Caller/Host implementations are boundary stand-ins;
MediaStoreProvider's Binder, permission, app-op and file-descriptor glue is not
exercised here.

Use --source-ref REF to show the state before this change: without the production
class there is no content://media provider and the run fails.
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
PRODUCTION = ('MediaStoreIndex',)
INPUTS = {
    'sqlite-jdbc-3.46.1.3.jar': (
        'https://repo1.maven.org/maven2/org/xerial/sqlite-jdbc/3.46.1.3/sqlite-jdbc-3.46.1.3.jar',
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


def client_scan_file(ref: str | None) -> str:
    relative = 'app/java/android/provider/MediaStore.java'
    if ref:
        text = subprocess.run(['git', 'show', f'{ref}:{relative}'], cwd=ROOT,
                              capture_output=True, text=True, check=True).stdout
    else:
        text = (ROOT / relative).read_text()
    start = text.index('public static Uri scanFile(ContentResolver resolver, File file)')
    return text[start:text.index('}', start) + 1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--java-home', default=os.environ.get('JAVA_HOME'))
    parser.add_argument('--source-ref', help='compile production classes from this Git revision')
    parser.add_argument('--cache-dir', type=Path,
                        default=Path(os.environ.get('RUNNER_TEMP', tempfile.gettempdir()))
                        / 'gesher-mediastore-contract-inputs')
    args = parser.parse_args()
    executable = lambda name: str(Path(args.java_home) / 'bin' / name) if args.java_home else name

    # MediaScannerConnection.scanFile reaches the provider only through MediaStore.scanFile.
    failed = False
    scan = client_scan_file(args.source_ref)
    if '"scan_file"' not in scan:
        print('FAIL MediaStore.scanFile does not call the provider: MediaScannerConnection.scanFile '
              'reports a null URI for every path', flush=True)
        failed = True

    sources = {}
    for name in PRODUCTION:
        source = production_source(args.source_ref, name)
        if source is None:
            print(f'FAIL {name}.java does not exist at {args.source_ref}: no content://media provider, '
                  'so ContentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, ...) fails '
                  'with IllegalArgumentException "Unknown URL content://media/external/images/media"',
                  flush=True)
            failed = True
            continue
        print(f'{name} SHA256 {hashlib.sha256(source.encode()).hexdigest()}', flush=True)
        sources[name] = source
    if failed:
        sys.exit(1)

    jar = fetch(args.cache_dir, 'sqlite-jdbc-3.46.1.3.jar')
    aosp = [fetch(args.cache_dir, name) for name in ('SQLiteQueryBuilder.java', 'SQLiteTokenizer.java')]
    with tempfile.TemporaryDirectory(prefix='gesher-mediastore-') as temporary:
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
        files += [str(p) for p in (ROOT / 'tests/mediastore/standins').rglob('*.java')]
        files.append(str(ROOT / 'tests/mediastore/MediaStoreHost.java'))
        files.append(str(ROOT / 'tests/mediastore/MediaStoreCrashChild.java'))
        classes = work / 'classes'
        subprocess.run([executable('javac'), '-nowarn', '-encoding', 'UTF-8', '-proc:none',
                        '-d', str(classes), '-cp', str(jar), *files], check=True, timeout=180)
        subprocess.run([executable('java'), '-cp', os.pathsep.join((str(classes), str(jar))),
                        'com.gesher.server.MediaStoreHost'], check=True, timeout=180)


if __name__ == '__main__':
    main()
