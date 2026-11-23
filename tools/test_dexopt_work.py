#!/usr/bin/env python3
"""Run unchanged production dexopt/queue/watch code; only Android/process edges are adapters."""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref', help='Prior Dexopt source for transactional negative control')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-dexopt-work-') as temporary:
        work = Path(temporary)
        sources = list((ROOT / 'tests/dexopt-host').rglob('*.java'))
        for name in ['Dexopt', 'DexoptWorkQueue', 'DexoptProfileWatch']:
            path = 'app/java/com/gesher/server/' + name + '.java'
            source = subprocess.check_output(['git', 'show', args.source_ref + ':' + path],
                    cwd=ROOT) if args.source_ref and name == 'Dexopt' else (ROOT / path).read_bytes()
            target = work / (name + '.java'); target.write_bytes(source); sources.append(target)
            print(name, hashlib.sha256(source).hexdigest(), flush=True)
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                '-d', str(work), *map(str, sources)], check=True, timeout=60)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx128m', '-cp', str(work),
                'com.gesher.server.DexoptWorkHost', str(work / 'device'),
                'negative' if args.source_ref else 'all'], check=True, timeout=45)
        if not args.source_ref:
            subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx128m', '-cp', str(work),
                    'com.gesher.server.DexoptProfileProgressHost', str(work / 'progress')], check=True, timeout=20)


if __name__ == '__main__':
    main()
