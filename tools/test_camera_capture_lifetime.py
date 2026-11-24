#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise full production WindowsCamera with deterministic native lifetime adapters.

Only nativeRead/nativeClose implementations are supplied by the fixture. A latch
holds admitted native calls while close and new readers race. No device is opened.
--source-ref runs the same gates against the former unsafe native-pointer lifetime.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    path = 'app/java/com/gesher/server/WindowsCamera.java'
    production = (subprocess.run(['git', 'show', args.source_ref + ':' + path], cwd=ROOT,
                                check=True, capture_output=True, text=True).stdout
                  if args.source_ref else (ROOT / path).read_text(encoding='utf-8'))
    adapters = {
        'private static native int nativeRead(long handle, byte[] nv12);':
            'private static int nativeRead(long handle, byte[] nv12) { return CameraCaptureLifetimeHost.read(handle, nv12); }',
        'private static native void nativeClose(long handle);':
            'private static void nativeClose(long handle) { CameraCaptureLifetimeHost.close(handle); }',
    }
    for boundary, adapter in adapters.items():
        if production.count(boundary) != 1:
            raise ValueError('camera native boundary changed: ' + boundary)
        production = production.replace(boundary, adapter)
    with tempfile.TemporaryDirectory(prefix='gesher-camera-lifetime-') as temporary:
        work = Path(temporary)
        source = work / 'WindowsCamera.java'; source.write_text(production, encoding='utf-8')
        size = work / 'Size.java'
        size.write_text('package android.util; public final class Size { public Size(int w, int h) {} }', encoding='utf-8')
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-source', '11',
                        '-target', '11', '-Xlint:-options', '-encoding', 'UTF-8', '-d', str(work),
                        str(source), str(size), str(ROOT / 'tests/java-performance/CameraCaptureLifetimeHost.java')],
                       check=True, timeout=60)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx24m',
                        '-cp', str(work), 'com.gesher.server.CameraCaptureLifetimeHost'],
                       check=True, timeout=90)


if __name__ == '__main__':
    main()
