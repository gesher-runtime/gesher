#!/usr/bin/env python3
"""Check Gesher's device identity (docs/device-identity.md).

Compiles and runs tests/native/device_identity_test.cc against the production
header (compat/posix/include/gesher/device_identity.h): SMBIOS parsing,
placeholder strings, the baseboard and Gesher fallbacks, virtual machines and
malformed tables. Then checks app/build.prop: Gesher's own brand, product,
device, board, hardware, bootloader and flavor, a fingerprint made of exactly
its fields in Android's format, no manufacturer/model (the PC's, set at
property-area creation) and no Pixel/Google identity.

The Windows read (GetSystemFirmwareTable) and the property loading for both
ANDROID_ROOT forms run in tests/windows/tracks/43-exec2.ps1.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# Build.java's derived fingerprint, as build/make's BUILD_FINGERPRINT:
# $(BRAND)/$(PRODUCT)/$(DEVICE):$(VERSION.RELEASE)/$(ID)/$(VERSION.INCREMENTAL):$(TYPE)/$(TAGS)
FINGERPRINT_FIELDS = ('ro.product.brand', 'ro.product.name', 'ro.product.device', 'ro.build.version.release',
                      'ro.build.id', 'ro.build.version.incremental', 'ro.build.type', 'ro.build.tags')


def build_prop(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, value = line.split('=', 1)
        key = key.strip().rstrip('?').strip()
        assert key not in values, f'{path}: {key} set twice'
        values[key] = value.strip()
    return values


def check_build_prop(path: Path) -> int:
    values = build_prop(path)
    checks = 0
    for key in ('ro.product.manufacturer', 'ro.product.model'):
        assert key not in values, f'{key} is the PC\'s (SMBIOS), not build.prop\'s'
        checks += 1
    for key in ('ro.product.brand', 'ro.product.name', 'ro.product.device', 'ro.product.board',
                'ro.hardware', 'ro.bootloader'):
        assert values.get(key) == 'gesher', f'{key}={values.get(key)!r}'
        checks += 1
    assert values.get('ro.build.flavor') == values['ro.product.name'] + '-' + values['ro.build.type'], 'flavor'
    checks += 1
    expected = '{}/{}/{}:{}/{}/{}:{}/{}'.format(*(values[key] for key in FINGERPRINT_FIELDS))
    assert values.get('ro.build.fingerprint') == expected, f'fingerprint {values.get("ro.build.fingerprint")!r} != {expected!r}'
    checks += 1
    # Each field is a single fingerprint token (no separators or whitespace).
    for key in FINGERPRINT_FIELDS:
        assert re.fullmatch(r'[A-Za-z0-9._-]+', values[key]), f'{key}={values[key]!r} breaks the fingerprint'
        checks += 1
    # The Android version, build and patch level stay android-16.0.0_r4's.
    assert values['ro.build.id'] == 'BP4A.251205.006' and values['ro.build.version.release'] == '16'
    assert values['ro.build.version.security_patch'] == '2025-12-05'
    checks += 2
    text = path.read_text(encoding='utf-8')
    for impersonation in ('shiba', 'Pixel', '=google', '=Google', 'google.com'):
        assert impersonation not in text, f'{impersonation!r} remains in {path}'
        checks += 1
    return checks


def check_windows_track_placeholders() -> int:
    """43-exec2.ps1 skips the SMBIOS comparison for exactly the header's placeholders."""
    header = (ROOT / 'compat/posix/include/gesher/device_identity.h').read_text(encoding='utf-8')
    track = (ROOT / 'tests/windows/tracks/43-exec2.ps1').read_text(encoding='utf-8')

    def c_list(name: str) -> list[str]:
        block = re.search(name + r'\[\] = \{(.*?)\};', header, re.S)
        assert block, f'{name} not found in device_identity.h'
        return re.findall(r'"([^"]*)"', block.group(1))

    def ps_list(name: str) -> list[str]:
        block = re.search(r'\$' + name + r' = @\((.*?)\)', track, re.S)
        assert block, f'${name} not found in 43-exec2.ps1'
        return re.findall(r"'([^']*)'", block.group(1))

    assert ps_list('identityPlaceholders') == c_list('kPlaceholders'), \
        '43-exec2.ps1 placeholders differ from device_identity.h kPlaceholders'
    assert ps_list('identityPlaceholderPrefixes') == c_list('kPrefixes'), \
        '43-exec2.ps1 placeholder prefixes differ from device_identity.h kPrefixes'
    return 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    checks = check_build_prop(ROOT / 'app/build.prop') + check_windows_track_placeholders()
    print(f'build.prop identity checks passed: {checks}', flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-device-identity-') as temporary:
        # Only the production header: compat/posix/include also replaces libc
        # headers, which a Linux host must not pick up.
        include = Path(temporary) / 'include/gesher'
        include.mkdir(parents=True)
        (include / 'device_identity.h').write_bytes(
            (ROOT / 'compat/posix/include/gesher/device_identity.h').read_bytes())
        flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
                 '-I', str(include.parent)]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        executable = Path(temporary) / ('test.exe' if os.name == 'nt' else 'test')
        subprocess.run(shlex.split(args.cxx) + flags + [str(ROOT / 'tests/native/device_identity_test.cc'),
                       '-o', str(executable)], check=True, timeout=120)
        return subprocess.run([str(executable)], timeout=60).returncode


if __name__ == '__main__':
    raise SystemExit(main())
