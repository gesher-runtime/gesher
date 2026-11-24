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
        (includes / 'environment.h').write_bytes(
            (root / 'compat/posix/include/gesher/environment.h').read_bytes())
        (includes / 'protected_startup.h').write_bytes(
            (root / 'compat/posix/include/gesher/protected_startup.h').read_bytes())
        source = tests / 'entry_test.cc'
        if args.baseline_ref:
            adapter = 'compat/art/dalvikvm_main.cc'
            baseline = subprocess.check_output(['git', 'show', f'{args.baseline_ref}:{adapter}'], cwd=root)
            (work / adapter).parent.mkdir(parents=True)
            (work / adapter).write_bytes(baseline)
            if b'"../binder/protected_client_startup.h"' in baseline:
                header = 'compat/binder/protected_client_startup.h'
                (work / header).parent.mkdir()
                (work / header).write_bytes(subprocess.check_output(
                    ['git', 'show', f'{args.baseline_ref}:{header}'], cwd=root))
            if b'"../usb_storage/protected_runtime_client.h"' in baseline:
                header = 'compat/usb_storage/protected_runtime_client.h'
                (work / header).parent.mkdir(exist_ok=True)
                (work / header).write_bytes(subprocess.check_output(
                    ['git', 'show', f'{args.baseline_ref}:{header}'], cwd=root))
            source = work / 'tests/art-temp/entry_test.cc'
            source.parent.mkdir(parents=True)
            source.write_bytes((tests / 'entry_test.cc').read_bytes())
        executable = work / 'entry-test'
        features = [] if args.baseline_ref else ['-DGESHER_ART_TEST_PROTECTED_STARTUP']
        subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', *features,
                        '-I', str(tests), '-I', str(work / 'include'),
                        str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
