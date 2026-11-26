#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run actual native profile-selection methods at declared Win32 API boundaries.

The host fixture controls environment, file attributes and GetFullPathNameW. It
checks selection and failures; real Windows path resolution/UI need Windows CI.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_install_metadata_comparison import method

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    production = (ROOT / 'compat/shell/gesher_launch.cc').read_text(encoding='utf-8')
    signatures = ('std::wstring Directory(', 'std::wstring Lower(',
                  'bool IsFile(', 'std::wstring GetDataDirectory(')
    exact = '\n'.join(method(production, signature) for signature in signatures)
    template = (ROOT / 'tests/portable/PortableLauncherHost.cc.in').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='gesher-portable-launcher-') as temporary:
        work = Path(temporary)
        source = work / 'launcher.cc'
        source.write_text(template.replace('@PRODUCTION_METHODS@', exact), encoding='utf-8')
        binary = work / ('launcher.exe' if os.name == 'nt' else 'launcher')
        subprocess.run([args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(source),
                        '-o', str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == '__main__':
    main()
