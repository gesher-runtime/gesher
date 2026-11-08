#!/usr/bin/env python3
"""Run actual entry-adapter policy with modeled Windows replies, not a Windows VM."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--baseline-ref')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    tests = root / 'tests/art-temp'
    with tempfile.TemporaryDirectory(prefix='gesher-temp-policy-') as temporary:
        work = Path(temporary)
        includes = work / 'include/gesher'
        includes.mkdir(parents=True)
        (includes / 'unc_paths.h').write_bytes(
            (root / 'compat/posix/include/gesher/unc_paths.h').read_bytes())
        source = tests / 'entry_test.cc'
        if args.baseline_ref:
            baseline = subprocess.check_output([
                'git', 'show', args.baseline_ref + ':compat/art/dalvikvm_main.cc'], cwd=root)
            old = work / 'dalvikvm_main.cc'
            old.write_bytes(baseline)
            source = work / 'entry_test.cc'
            source.write_text((tests / 'entry_test.cc').read_text().replace(
                '#include "../../compat/art/dalvikvm_main.cc"', '#include "dalvikvm_main.cc"'))
        executable = work / 'entry-test'
        subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I', str(tests), '-I', str(work / 'include'),
                        str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
