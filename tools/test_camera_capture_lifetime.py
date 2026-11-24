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

COLLABORATOR_STUBS = {
    'CameraImageFallback.java': 'package com.gesher.server; final class CameraImageFallback {'
        ' static boolean enabled() { return false; }'
        ' static byte[] toNv12(java.io.File f, int w, int h) throws java.io.IOException {'
        ' throw new java.io.IOException("stub"); } }',
    'IntentChooser.java': 'package com.gesher.server; final class IntentChooser {'
        ' interface Dialog { boolean available(); int choose(String r, String t, String[] l,'
        ' boolean[] e, java.util.function.BooleanSupplier a); }'
        ' static final Dialog HOST = new Dialog() { public boolean available() { return false; }'
        ' public int choose(String r, String t, String[] l, boolean[] e,'
        ' java.util.function.BooleanSupplier a) { return -1; } }; }',
    'DocumentPicker.java': 'package com.gesher.server; final class DocumentPicker { static final int OPEN = 0; }',
    'WindowsHost.java': 'package com.gesher.server; final class WindowsHost {'
        ' static String[] pickDocuments(int m, String t, String s, String i, String f, String x,'
        ' boolean mu, boolean l, int p, java.util.function.BooleanSupplier a) { return null; } }',
    'Zygote.java': 'package com.gesher.server; final class Zygote {'
        ' static String fromWindowsPath(String p) { return p; } }',
    'Process.java': 'package android.os; public final class Process {'
        ' public static int myPid() { return 0; } }',
}


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
        # The image-picker source's collaborators (CameraImageFallback setting off): the
        # lifetime gates open no device and never reach them.
        stubs = []
        for name, text in COLLABORATOR_STUBS.items():
            stub = work / name
            stub.write_text(text, encoding='utf-8')
            stubs.append(str(stub))
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-source', '11',
                        '-target', '11', '-Xlint:-options', '-encoding', 'UTF-8', '-d', str(work),
                        str(source), str(size), *stubs,
                        str(ROOT / 'tests/java-performance/CameraCaptureLifetimeHost.java')],
                       check=True, timeout=60)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx24m',
                        '-cp', str(work), 'com.gesher.server.CameraCaptureLifetimeHost'],
                       check=True, timeout=90)


if __name__ == '__main__':
    main()
