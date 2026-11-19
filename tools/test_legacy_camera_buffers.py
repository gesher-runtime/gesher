#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run the real legacy preview loop/frame packing at declared camera/JNI boundaries.

The production Session, previewLoop and frame methods are unchanged. The host
capture supplies deterministic frames and the native NV21 adapter counts calls.
Byte/padding/buffer identity tests cover the production Java packing and admission;
this does not emulate a Media Foundation device or JNI conversion implementation.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

from test_install_metadata_comparison import method

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref')
    parser.add_argument('--case', choices=('all', 'behavior', 'measurement'), default='all')
    parser.add_argument('--measure', action='store_true', help='omit new performance gates')
    args = parser.parse_args()
    path = 'app/java/com/gesher/server/LegacyCamera.java'
    production = (subprocess.run(['git', 'show', args.source_ref + ':' + path], cwd=ROOT,
                                check=True, capture_output=True, text=True).stdout
                  if args.source_ref else (ROOT / path).read_text(encoding='utf-8'))
    snippets = [method(production, marker) for marker in (
        'private static final class Session',
        'private static void post(Session s, int what, int arg1, int arg2, Object obj)',
        'public static void setHasPreviewCallback(Camera camera, boolean installed, boolean manual)',
        'public static void addCallbackBuffer(Camera camera, byte[] buffer, int msgType)',
        'private static void previewLoop(Session s, WindowsCamera.Capture capture, Size size)',
        'private static byte[] frame(Session s, byte[] nv12, int cw, int ch, int w, int h')]
    # The frame signature gained a manual-admission argument; match its exact start.
    # Both signatures are selected by the same unique method-name/type prefix.
    template = (ROOT / 'tests/java-performance/LegacyPreviewHost.java.in').read_text(encoding='utf-8')
    if template.count('@PRODUCTION@') != 1:
        raise ValueError('legacy preview host template changed')
    with tempfile.TemporaryDirectory(prefix='gesher-legacy-preview-') as temporary:
        work = Path(temporary)
        java = work / 'LegacyPreviewHost.java'
        java.write_text(template.replace('@PRODUCTION@', '\n'.join(snippets)), encoding='utf-8')
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-source', '11',
                        '-target', '11', '-Xlint:-options', '-encoding', 'UTF-8', '-d', str(work),
                        str(java)], check=True, timeout=60)
        for case in ['behavior', 'measurement'] if args.case == 'all' else [args.case]:
            subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx24m',
                            '-cp', str(work), 'com.gesher.server.LegacyPreviewHost', case,
                            'measure' if args.measure else 'gate'], check=True, timeout=90)


if __name__ == '__main__':
    main()
