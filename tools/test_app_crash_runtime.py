#!/usr/bin/env python3
"""Exercise the real Windows/ART crash-to-notice path on a machine without Bluetooth.

Requires a complete built runtime (including the new launcher/jar) and crashnotice.apk.
Uses standalone Android services and an isolated profile, not the user's app data.
"""
import argparse
import ctypes
from ctypes import wintypes as w
import os
from pathlib import Path
import shutil
import subprocess
import time


def notices(package, log_path):
    u = ctypes.WinDLL('user32')
    callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    u.EnumWindows.argtypes = [callback, w.LPARAM]
    u.EnumChildWindows.argtypes = [w.HWND, callback, w.LPARAM]
    u.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM,
                                    w.UINT, w.UINT, ctypes.POINTER(ctypes.c_size_t)]
    u.SendMessageTimeoutW.restype = w.LPARAM
    u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
    u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
    found = []
    @callback
    def visit(hwnd, unused):
        name = ctypes.create_unicode_buffer(256)
        u.GetClassNameW(hwnd, name, len(name))
        if name.value != '#32770':
            return True
        labels = []
        @callback
        def child(ch, unused):
            text = ctypes.create_unicode_buffer(16384)
            result = ctypes.c_size_t()
            # The host prompt may use an EDIT for long text. GetWindowTextW
            # cannot retrieve that control's contents across process boundaries.
            if u.SendMessageTimeoutW(ch, 0x000D, len(text), ctypes.addressof(text),
                    0x3, 1000, ctypes.byref(result)):  # SMTO_BLOCK | SMTO_ABORTIFHUNG
                labels.append(text.value)
            return True
        u.EnumChildWindows(hwnd, child, 0)
        text = '\n'.join(labels)
        if package in text and log_path in text:
            found.append(text)
            u.PostMessageW(hwnd, 0x10, 0, 0)
        return True
    u.EnumWindows(visit, 0)
    return found


def run(runtime, fixture, output, enabled, visible):
    profile = output / (('visible' if visible else 'hidden')
                        + ('-enabled' if enabled else '-disabled'))
    profile.mkdir(parents=True, exist_ok=True)
    for part in ('system/framework', 'system/etc', 'system/fonts', 'data', 'art', 'i18n/etc/icu'):
        (profile / part).mkdir(parents=True, exist_ok=True)
    shutil.copy2(runtime / 'build.prop', profile / 'system/build.prop')
    shutil.copy2(runtime / 'framework-res.apk', profile / 'system/framework/framework-res.apk')
    for p in (runtime / 'fonts').glob('*'):
        if p.is_file():
            shutil.copy2(p, profile / 'system/fonts' / p.name)
    shutil.copy2(runtime / 'fonts/fonts.xml', profile / 'system/etc/fonts.xml')
    shutil.copy2(next(runtime.glob('icudt*l.dat')), profile / 'i18n/etc/icu' / next(runtime.glob('icudt*l.dat')).name)
    # Gesher's drive-less guest paths are resolved on the runtime's drive.
    def nd(p):
        path = str(p.resolve()).replace('\\', '/')
        if path[:2].lower() != str(runtime)[:2].lower():
            return '/mnt/' + path[0].lower() + path[2:]
        return path[2:]
    env = {k: v for k, v in os.environ.items() if not k.startswith(('GESHER_', 'ANDROID_'))}
    env.update(ANDROID_ROOT=nd(profile / 'system'), ANDROID_DATA=nd(profile / 'data'),
               ANDROID_ART_ROOT=nd(profile / 'art'), ANDROID_I18N_ROOT=nd(profile / 'i18n'),
               ANDROID_TZDATA_ROOT=nd(runtime / 'tzdata'), GESHER_LOGS=nd(profile / 'logs'),
               GESHER_LOCALES='he', GESHER_SHORTCUTS='0', GESHER_CRASH_DIALOGS='1' if enabled else '0')
    bcp = ':'.join(nd(runtime / (p + '.jar')) for p in ('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app'))
    cmd = [str(runtime / 'dalvikvm.exe'), '-Xbootclasspath:' + bcp,
           '-Ximage:' + nd(runtime / 'framework/boot.art'), '-Xint', '-Xusejit:false',
           'com.gesher.app.Main', '--window' if visible else '--hidden-window', nd(fixture)]
    report = profile / 'logs/com.gesher.crashnotice.crash.log'
    with (profile / 'runtime.log').open('wb') as log:
        p = subprocess.Popen(cmd, cwd=runtime, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            assert p.wait(timeout=45) == 1, 'original failure exit code changed'
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
    trace = (profile / 'runtime.log').read_text(encoding='utf-8', errors='replace')
    assert "getBluetoothLeScanner()' on a null object reference" in trace, trace[-5000:]
    assert 'first frame drawn' in trace, 'fixture did not draw an application frame'
    found = []
    # The ART process has exited; its independently owned notice must still work.
    expected_notice = enabled and visible
    deadline = time.monotonic() + (5 if enabled else 1)
    while time.monotonic() < deadline and not found:
        found += notices('com.gesher.crashnotice', str(report))
        time.sleep(.05)
    if expected_notice:
        assert len(found) == 1 and 'Bluetooth' in found[0] and 'Windows' in found[0], found
        evidence = report.read_text(encoding='utf-8')
        assert 'getBluetoothLeScanner' in evidence
        import re
        events = re.findall(r'>>> Gesher crash event ([0-9a-f-]+)', evidence)
        assert events and events[-1] in found[0], 'notice does not identify its report entry'
    else:
        assert not found, 'dialog suppression failed'
        assert not report.exists(), 'suppressed crash created a desktop-notice report'
    print('PASS: actual Windows/ART missing-radio crash, exit 1, '
          + ('visible' if visible else 'hidden') + ' window, notice '
          + ('survived process exit' if expected_notice else 'suppressed'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    for enabled, visible in ((True, True), (False, True), (True, False)):
        run(args.runtime.resolve(), args.fixture.resolve(), args.output.resolve(), enabled, visible)
