#!/usr/bin/env python3
"""Native Windows broker pipe churn and in-flight participant ownership.

Compiles the whole production broker with test-only allocation accounting.
--source-ref runs the same churn assertions against an earlier broker.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--aosp-dir', type=Path, required=True)
    parser.add_argument('--source-ref')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.compile_only and args.output is None:
        parser.error('--compile-only requires --output')
    if os.name != 'nt' and not args.compile_only:
        parser.error('This test executes real Windows named pipes')
    with tempfile.TemporaryDirectory(prefix='gesher-broker-life-') as directory:
        work = Path(directory)
        relative = 'compat/binder/binderd.cc'
        broker = ROOT / relative
        if args.source_ref:
            broker = work / relative
            broker.parent.mkdir(parents=True)
            broker.write_bytes(subprocess.check_output(['git', 'show', args.source_ref + ':' + relative], cwd=ROOT))
            for name in ('binder_wire.h', 'binder_sg.h'):
                target = work / 'compat/posix/src' / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / 'compat/posix/src' / name).read_bytes())
        source = work / 'lifetime.cc'
        source.write_text((ROOT / 'tests/native/binder_lifetime_test.cc').read_text().replace('@BROKER_SOURCE@', broker.as_posix()))
        binary = args.output.resolve() if args.output else work / 'lifetime.exe'
        binary.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-static', '-pthread',
                       '-I' + str(ROOT / 'compat/posix/include'), '-idirafter', str(args.aosp_dir / 'bionic/libc/kernel/uapi'),
                       str(source), '-o', str(binary)], check=True, timeout=90)
        if not args.compile_only:
            subprocess.run([str(binary)], check=True, timeout=45)


if __name__ == '__main__':
    main()
