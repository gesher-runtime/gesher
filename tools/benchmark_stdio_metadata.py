#!/usr/bin/env python3
"""A/B the historical ordinary-file Fgetc cache against its predecessor.

Uses glibc FILE locking in place of UCRT, with the device predicate false in
both variants. File setup is outside timing and every byte/EOF is checked.
This isolates the metadata change; it is not a Windows application benchmark.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(source, first, end):
    begin = source.index(first)
    return source[begin:source.index(end, begin)]


def bodies(source):
    return '\n'.join([
        section(source, 'constexpr uint32_t kWeof', 'class StreamLock'),
        section(source, 'int BIONIC_SYSV GetcUnlocked(', 'int BIONIC_SYSV PutcUnlocked('),
        section(source, 'int BIONIC_SYSV Fgetc(', 'int BIONIC_SYSV Getchar('),
    ])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', default='8b316aebf09ab46d5407e955bf86ed748a1e739f')
    parser.add_argument('--candidate-ref', default='972c2b42b40c4ac799e147f5e352d07ba615d0fb')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    filename = 'compat/bionic/stdio_bionic.cc'
    baseline = subprocess.check_output(['git', 'show', args.baseline_ref + ':' + filename], cwd=ROOT, text=True)
    current = subprocess.check_output(['git', 'show', args.candidate_ref + ':' + filename], cwd=ROOT, text=True)
    template = (ROOT / 'tests/native/stdio_metadata_benchmark.cc.in').read_text()
    for marker, source in [('@BEFORE@', baseline), ('@AFTER@', current)]:
        code = bodies(source)
        print(marker, hashlib.sha256(code.encode()).hexdigest(), flush=True)
        template = template.replace(marker, code)
    with tempfile.TemporaryDirectory(prefix='gesher-stdio-metadata-bench-') as directory:
        source, binary = Path(directory) / 'bench.cc', Path(directory) / 'bench'
        source.write_text(template)
        subprocess.run(shlex.split(args.cxx) + ['-std=c++20', '-O2', '-pthread', str(source), '-o', str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == '__main__':
    main()
