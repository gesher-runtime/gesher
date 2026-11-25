#!/usr/bin/env python3
"""Run the production SDP-lookup section of BluetoothManagerService around fake native calls."""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
START = '    // --- SDP lookups '
END = '    // --- the app side'


def section(source: str) -> str:
    if source.count(START) != 1 or source.count(END) != 1:
        raise ValueError('production SDP section boundary changed')
    return source[source.index(START):source.index(END)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    relative = 'app/java/com/gesher/server/BluetoothManagerService.java'
    source = (subprocess.check_output(['git', 'show', args.source_ref + ':' + relative], cwd=ROOT, text=True)
              if args.source_ref else (ROOT / relative).read_text())
    methods = section(source)
    template = (ROOT / 'tests/bluetooth/BluetoothSdpHost.java.in').read_text()
    with tempfile.TemporaryDirectory(prefix='gesher-bluetooth-sdp-') as tmp:
        work = Path(tmp)
        java = work / 'BluetoothSdpHost.java'
        java.write_text(template.replace('// PRODUCTION_SDP_METHODS', methods))
        subprocess.run([str(args.java_home / 'bin/javac'), '-d', str(work), str(java)], check=True, timeout=90)
        subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work), 'BluetoothSdpHost'],
                       check=True, timeout=60)


if __name__ == '__main__':
    main()
