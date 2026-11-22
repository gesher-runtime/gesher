#!/usr/bin/env python3
"""Compile actual WinRT owner-checked removal with explicit COM fixture boundaries.

Runs on Windows only and never shows notifications or accesses live toast history.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', required=True)
    p.add_argument('--output', type=Path)
    p.add_argument('--compile-only', action='store_true')
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-toast-history-') as tmp:
        exe = a.output.resolve() if a.output else Path(tmp) / 'toast_history_test.exe'
        exe.parent.mkdir(parents=True, exist_ok=True)
        arguments = Path(tmp) / 'toast_identity_arguments.obj'
        subprocess.run([a.cc, '-x', 'c++', '-std=c++20', '-fno-exceptions', '-c',
            str(ROOT / 'tests/notification-identity/toast_identity_arguments_test.cc'), '-o', str(arguments)], check=True)
        subprocess.run([a.cc, '-std=c11', '-static', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            str(ROOT / 'tests/notification-identity/toast_history_test.c'), str(arguments),
            '-o', str(exe), '-lole32', '-luuid'], check=True)
        if not a.compile_only: subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()
