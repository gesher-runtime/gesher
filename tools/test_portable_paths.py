#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Execute unchanged Installer path methods with real host filesystem operations.

The Device.dataDir boundary selects an isolated profile. This checks copied and
moved profiles and canonical/link boundaries; it does not parse APKs or run ART.
--source-ref runs the identical fixture against an earlier implementation.
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
    parser.add_argument('--java-home', type=Path,
                        default=Path(os.environ['JAVA_HOME']) if os.environ.get('JAVA_HOME') else None)
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    path = 'app/java/com/gesher/server/Installer.java'
    source = (subprocess.check_output(['git', 'show', args.source_ref + ':' + path],
                                     cwd=ROOT, text=True) if args.source_ref
              else (ROOT / path).read_text(encoding='utf-8'))
    exact = '\n'.join(method(source, signature) for signature in
                      ('private static File codeRoot()', 'static String resolveCodePath('))
    with tempfile.TemporaryDirectory(prefix='gesher-portable-paths-') as temporary:
        work = Path(temporary)
        generated = work / 'Installer.java'
        generated.write_text('import java.io.*; import java.nio.file.*;\n'
                             'final class Installer {\n' + exact + '\n}\n', encoding='utf-8')
        javac = str(args.java_home / 'bin/javac') if args.java_home else 'javac'
        java = str(args.java_home / 'bin/java') if args.java_home else 'java'
        subprocess.run([javac, '-encoding', 'UTF-8', '-d', str(work), str(generated),
                        str(ROOT / 'tests/portable/PortablePathsHost.java')], check=True, timeout=60)
        subprocess.run([java, '-cp', str(work), 'PortablePathsHost', str(work)],
                       check=True, timeout=30)


if __name__ == '__main__':
    main()
