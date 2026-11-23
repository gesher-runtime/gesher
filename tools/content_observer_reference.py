#!/usr/bin/env python3
"""Prepare an Android 16 r4 observer-selection oracle from verified upstream source.

The seven registration/URI-selection methods are compiled unchanged. Only the
Binder-death/logging/collector boundary and the entry data holder are supplied;
none of those adapters chooses which observer matches. This is an AOSP source
component comparison, not execution of an Android device or a complete service.
"""
from __future__ import annotations

import hashlib
from pathlib import Path

SHA256 = '0efb8bfa6fc2fd39a15865b3ad27357253f2424e3d882581062ba9485c5510e2'
URL = ('https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/'
       'services/core/java/com/android/server/content/ContentService.java')
SIGNATURES = (
    '        public static String getUriSegment(',
    '        public static int countUriSegments(',
    '        public void addObserverLocked(Uri uri, IContentObserver observer,',
    '        private void addObserverLocked(Uri uri, int index, IContentObserver observer,',
    '        private void collectMyObserversLocked(',
    '        public void collectObserversLocked(Uri uri, int index,',
    '        public void collectObserversLocked(Uri uri, int segmentCount, int index,',
)


def prepare(source_path: Path, destination: Path) -> Path:
    source_bytes = source_path.read_bytes()
    digest = hashlib.sha256(source_bytes).hexdigest()
    if digest != SHA256:
        raise ValueError(f'Unqualified ContentService oracle source {digest}; expected {SHA256}: {URL}')
    source = source_bytes.decode('utf-8')
    methods = []
    for signature in SIGNATURES:
        if source.count(signature) != 1:
            raise ValueError('Upstream oracle method boundary changed: ' + signature)
        start = source.index(signature)
        brace = source.index('{', start)
        depth = 1
        end = brace + 1
        while depth:
            if source[end] == '{': depth += 1
            elif source[end] == '}': depth -= 1
            end += 1
        methods.append(source[start:end])
    # Retain the source's Apache license and attribution in the generated oracle.
    license_header = source[:source.index('*/') + 2]
    template = (Path(__file__).resolve().parents[1] /
                'tests/java-performance/ContentObserverReferenceHost.java.in').read_text(encoding='utf-8')
    if template.count('/* UPSTREAM_SELECTION_METHODS */') != 1:
        raise ValueError('Oracle template boundary changed')
    destination.write_text(license_header + '\n' + template.replace(
        '/* UPSTREAM_SELECTION_METHODS */', '\n\n'.join(methods)), encoding='utf-8')
    print(f'Android observer reference SHA256 {digest}; seven unchanged selection methods', flush=True)
    return destination
