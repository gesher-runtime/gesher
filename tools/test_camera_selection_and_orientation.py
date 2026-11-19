#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Verify production camera selection, orientation and feature reporting helpers."""
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
    args = parser.parse_args()

    wc_src = (ROOT / 'app/java/com/gesher/server/WindowsCamera.java').read_text(encoding='utf-8')
    cs_src = (ROOT / 'app/java/com/gesher/server/CameraService.java').read_text(encoding='utf-8')
    pm_src = (ROOT / 'app/java/com/gesher/server/PackageManagerService.java').read_text(encoding='utf-8')

    wc_parts = '\n'.join([
        method(wc_src, 'public static final class Device'),
        method(wc_src, 'static List<Device> resolveDevices('),
        method(wc_src, 'static String selectionKey(String windowsId)'),
    ])
    cs_parts = '\n'.join([
        method(cs_src, 'static int sensorOrientation()'),
    ])
    pm_parts = '\n'.join([
        method(pm_src, 'static void addCameraFeatures('),
        method(pm_src, 'private static void addFeature('),
    ])

    template = (ROOT / 'tests/java-performance/CameraSelectionOrientationHost.java.in').read_text(
        encoding='utf-8'
    )
    rendered = (
        template.replace('@WINDOWS_CAMERA@', wc_parts)
        .replace('@CAMERA_ORIENTATION@', cs_parts)
        .replace('@PM_CAMERA_FEATURES@', pm_parts)
    )
    with tempfile.TemporaryDirectory(prefix='gesher-camera-select-') as temporary:
        work = Path(temporary)
        java = work / 'CameraSelectionOrientationHost.java'
        java.write_text(rendered, encoding='utf-8')
        subprocess.run(
            [
                str(args.java_home / 'bin/javac'),
                '-J-XX:-UsePerfData',
                '-source', '11',
                '-target', '11',
                '-Xlint:-options',
                '-encoding', 'UTF-8',
                '-d', str(work),
                str(java),
            ],
            check=True,
            timeout=60,
        )
        subprocess.run(
            [
                str(args.java_home / 'bin/java'),
                '-XX:-UsePerfData',
                '-Xmx24m',
                '-cp', str(work),
                'com.gesher.server.CameraSelectionOrientationHost',
            ],
            check=True,
            timeout=60,
        )


if __name__ == '__main__':
    main()
