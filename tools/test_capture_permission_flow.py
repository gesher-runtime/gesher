#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Execute bundled camera permission/lifecycle methods at explicit UI/device boundaries."""
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
    source = (ROOT / 'app/gesher-camera/java/com/gesher/camera/CaptureActivity.java').read_text(
            encoding='utf-8')
    signatures = ('protected void onResume(', 'protected void onPause(',
                  'private void requestCameraAccess(', 'public void onRequestPermissionsResult(',
                  'private void startCamera(', 'private boolean canCapture(', 'private void capture(',
                  'private synchronized void done(', 'protected void onDestroy(')
    production = '\n\n'.join(method(source, signature) for signature in signatures)
    template = (ROOT / 'tests/camera-permission/CapturePermissionFlowHost.java.in').read_text(
            encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='gesher-camera-consent-') as temporary:
        work = Path(temporary)
        java = work / 'CapturePermissionFlowHost.java'
        java.write_text(template.replace('@PRODUCTION@', production), encoding='utf-8')
        subprocess.run([str(args.java_home / 'bin/javac'), '--release', '17', '-encoding', 'UTF-8',
                        '-d', str(work), str(java)], check=True, timeout=30)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-cp', str(work),
                        'com.gesher.camera.CapturePermissionFlowHost'], check=True, timeout=30)


if __name__ == '__main__':
    main()
