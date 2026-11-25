#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run exact production metadata discovery/comparison methods with real file I/O.

Android XML/signature validation is outside this fixture. The constructor,
auxiliary filter, comparison and its helper are copied unchanged. File-open and
readAllBytes boundaries count actual bytes, requests and JVM allocations.
--source-ref enables identical baseline behavior/performance controls.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source: str, marker: str) -> str:
    if source.count(marker) != 1:
        raise ValueError('production method boundary changed: ' + marker)
    start = source.index(marker)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[start:end]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref')
    parser.add_argument('--case', choices=('all', 'behavior', 'measurement', 'large'), default='all')
    parser.add_argument('--measure', action='store_true', help='omit new performance gates')
    args = parser.parse_args()
    path = 'app/java/com/gesher/server/InstallSessionMetadata.java'
    production = (subprocess.run(['git', 'show', args.source_ref + ':' + path], cwd=ROOT,
                                check=True, capture_output=True, text=True).stdout
                  if args.source_ref else (ROOT / path).read_text(encoding='utf-8'))
    fields = []
    for name in ('files', 'compared'):
        matches = re.findall(r'private final List<File> ' + name + r';', production)
        if len(matches) != 1:
            raise ValueError('production metadata field boundary changed')
        fields += matches
    constant = re.findall(r'static final String APP_METADATA = [^;]+;', production)
    if len(constant) != 1:
        raise ValueError('production metadata constant changed')
    markers = ['InstallSessionMetadata(File canonical)', 'static boolean auxiliary(String name)',
               'boolean matches(File installed)']
    if 'private static boolean sameContents(' in production:
        markers.append('private static boolean sameContents(File file, File previous)')
    exact = '\n'.join(fields + constant + [method(production, marker) for marker in markers])
    # Instrument only the file-I/O boundary, leaving discovery and equality intact.
    exact = exact.replace('new FileInputStream(', 'new CountingFileInputStream(')
    exact = exact.replace('Files.readAllBytes(', 'MetadataComparisonHost.readAllBytes(')
    generated = ('package com.gesher.server;\nimport java.io.*;\nimport java.nio.file.Files;\n'
                 'import java.util.*;\nfinal class InstallSessionMetadata {\n' + exact + '\n}\n')
    with tempfile.TemporaryDirectory(prefix='gesher-metadata-compare-') as temporary:
        work = Path(temporary)
        java = work / 'InstallSessionMetadata.java'
        java.write_text(generated, encoding='utf-8')
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-source', '11',
                        '-target', '11', '-Xlint:-options', '-encoding', 'UTF-8', '-d', str(work),
                        str(java), str(ROOT / 'tests/java-performance/MetadataComparisonHost.java')],
                       check=True, timeout=60)
        cases = ['behavior', 'measurement', 'large'] if args.case == 'all' else [args.case]
        for case in cases:
            subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx24m',
                            '-cp', str(work), 'com.gesher.server.MetadataComparisonHost', case,
                            'measure' if args.measure else 'gate'], check=True, timeout=90)


if __name__ == '__main__':
    main()
