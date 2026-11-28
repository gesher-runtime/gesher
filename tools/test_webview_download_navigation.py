#!/usr/bin/env python3
"""Exercise production request/frame/loader identity parsing with real Android JSON.

Host execution checks the production Java state machine; Windows WebView/ART
fixtures separately exercise actual CDP delivery and application callbacks.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', required=True, type=Path)
    parser.add_argument('--json-classes', required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-network-identity-') as directory:
        work = Path(directory)
        import os
        subprocess.run([str(args.java_home / 'bin/javac'), '--release', '17', '-cp', str(args.json_classes),
            '-d', str(work), str(ROOT / 'app/java/com/gesher/app/webview/NetworkNavigationErrors.java'),
            str(ROOT / 'tests/webview/NetworkNavigationErrorsTest.java')], check=True, timeout=120)
        subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work) + os.pathsep + str(args.json_classes),
            'com.gesher.app.webview.NetworkNavigationErrorsTest'], check=True, timeout=30)

if __name__ == '__main__':
    main()
