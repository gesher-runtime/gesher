#!/usr/bin/env python3
"""Run the production Mobile Broadband adapter against isolated COM calls under ASan/UBSan.

tests/windows-cellular/native_host.cc supplies synthetic WWAN interfaces through an
explicit Win32/COM boundary; compat/net/windows_cellular.cc is compiled unchanged.
Negative controls mutate one production contract (reads and the connection-mode write)
and require the fixture to catch it.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CONTROLS = {
    # Interface-capability strings belong to the caller and must all be freed.
    'leak-capability-strings': ('    if (text) api.free_string(text);\n', '    (void)text;\n',
                                'Every BSTR is freed'),
    # A thread already in another COM apartment must not be uninitialized.
    'uninitialize-foreign-apartment': ('COM is usable.\n  bool balance = SUCCEEDED(initialized);',
                                       'COM is usable.\n  bool balance = true;',
                                       'Another apartment is never uninitialized'),
    # Airplane mode (the software radio) and a hardware switch must not be confused.
    'swap-radio-states': ('radio.value->get_SoftwareRadioState(&software)',
                          'radio.value->get_HardwareRadioState(&software)',
                          'Software radio states are reported in order'),
    # Write path: a disconnect Windows refuses must not leave the new mode behind.
    'keep-mode-after-refused-disconnect': (
        'if (out.rfind("error", 0) == 0 && FAILED(result = profile.value->UpdateProfile(xml.c_str()))) {',
        'if (false) {',
        'and restores the previous mode'),
    # Write path: auto-home (roaming off) connects at home; it must not disconnect there.
    'auto-home-disconnects-at-home': (
        'mode == Wide("auto-home") ? registered && !roaming : false;',
        'mode == Wide("auto-home") ? false : false;',
        'auto-home at home keeps the connection'),
    # Write path: an unchanged mode is a no-op, not a re-enforcement of the connection.
    'enforce-unchanged-mode': (
        '} else if (previous == mode) {',
        '} else if (false) {',
        'Unchanged mode, already connected'),
    # Write path: a refused restore must be reported, not hidden behind "error disconnect".
    'hide-refused-restore': (
        'out = line("error", "rollback", result);',
        '(void)result;',
        'A refused restore is reported'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--cxx', default='c++', help='C++ compiler (default: c++)')
    parser.add_argument('--negative-control', choices=sorted(CONTROLS))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='windows-cellular-native-') as temporary:
        output = Path(temporary) / 'cellular-native'
        extra = []
        if args.negative_control:
            before, after, _ = CONTROLS[args.negative_control]
            source = (ROOT / 'compat/net/windows_cellular.cc').read_text()
            if source.count(before) != 1:
                raise AssertionError('Negative control target changed')
            negative = Path(temporary) / 'negative.cc'
            negative.write_text(source.replace(before, after))
            extra = ['-DGESHER_CELLULAR_SOURCE="' + str(negative) + '"']
        platform = 'win32' if (args.java_home / 'include/win32').is_dir() else 'linux'
        subprocess.run([args.cxx, '-std=c++17', '-g', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter',
                        '-Wno-writable-strings', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I' + str(ROOT / 'tests/windows-cellular/include'),
                        '-I' + str(args.java_home / 'include'),
                        '-I' + str(args.java_home / 'include' / platform),
                        *extra, str(ROOT / 'tests/windows-cellular/native_host.cc'), '-o', str(output)],
                       check=True)
        result = subprocess.run([str(output)], timeout=30, capture_output=bool(args.negative_control),
                                text=True)
        if args.negative_control:
            expected = CONTROLS[args.negative_control][2]
            if result.returncode == 0 or expected not in result.stderr:
                raise AssertionError('Negative control did not fail at the intended production contract: '
                                     + result.stderr)
            print('Caught native negative control:', args.negative_control)
        else:
            result.check_returncode()


if __name__ == '__main__':
    main()
