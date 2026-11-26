# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Robolectric's android-all 16 jar from Maven Central, pinned by SHA-256.

Host contracts compile unchanged production classes against AOSP's own
android.content/android.net value classes (Intent, IntentFilter, Uri, Bundle,
ResolveInfo, ...) from this jar: android-16 framework bytecode, run on JDK 21.
Only pure-Java classes are usable; anything reaching a native method is not.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import urllib.request

VERSION = '16-robolectric-13921718'
URL = ('https://repo1.maven.org/maven2/org/robolectric/android-all/' + VERSION
       + '/android-all-' + VERSION + '.jar')
SHA256 = '8b74a0a137330658d2f33f0dc715d42734f74ba8b2d7014fc2e95aa40d3f682d'


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def jar(explicit: Path | None = None) -> Path:
    """The verified jar: `explicit`, else the cache, else a fresh download."""
    if explicit is not None:
        if _sha256(explicit) != SHA256:
            raise SystemExit(f'{explicit}: SHA-256 is not android-all {VERSION}')
        return explicit
    cache = Path(os.environ.get('GESHER_ANDROID_ALL_CACHE',
                                Path.home() / '.cache' / 'gesher'))
    path = cache / ('android-all-' + VERSION + '.jar')
    if path.exists() and _sha256(path) == SHA256:
        return path
    cache.mkdir(parents=True, exist_ok=True)
    partial = path.with_suffix('.part')
    with urllib.request.urlopen(URL, timeout=300) as response, partial.open('wb') as out:
        while True:
            block = response.read(1 << 20)
            if not block:
                break
            out.write(block)
    if _sha256(partial) != SHA256:
        partial.unlink()
        raise SystemExit(f'{URL}: SHA-256 mismatch')
    partial.replace(path)
    return path
