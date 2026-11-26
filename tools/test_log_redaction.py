#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run the production LogRedaction (persisted stdout/stderr, crash reports, upgrade scrub)
against the credential vectors the native sink's contracts also use.

Real file I/O and PrintStream encoding are retained. The native record writer is covered by
tools/test_structured_logging.py; app processes on Windows by tests/windows/tracks/45-exec3.ps1.
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
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-log-redaction-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-source', '11',
                        '-target', '11', '-Xlint:-options', '-encoding', 'UTF-8', '-d', str(classes),
                        str(ROOT / 'app/java/com/gesher/app/LogRedaction.java'),
                        str(ROOT / 'tests/logging/LogRedactionTest.java')], check=True, timeout=60)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx64m',
                        '-Dfile.encoding=UTF-8', '-cp', str(classes), 'com.gesher.app.LogRedactionTest',
                        str(ROOT / 'tests/logging/redaction_vectors.inc'), str(work)],
                       check=True, timeout=60)


if __name__ == '__main__':
    main()
