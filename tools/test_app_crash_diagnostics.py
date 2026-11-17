#!/usr/bin/env python3
"""Run production Java crash classification/reporting with explicit host boundaries.

With --launcher EXE on Windows, also exercise the actual localized Win32 dialog,
argument parsing and dismissal. This does not substitute for Windows/ART app tests.
"""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def host_test():
    stubs = {
        'android/app/ActivityThread.java': 'package android.app; public class ActivityThread { public static Application currentApplication() { return new Application(); } }',
        'android/app/Application.java': 'package android.app; public class Application { public String getPackageName() { return "test.pkg"; } }',
        'com/gesher/app/shell/Shell.java': 'package com.gesher.app.shell; public class Shell { public static boolean exists(String p) { return true; } }',
        'com/gesher/app/window/WindowSession.java': 'package com.gesher.app.window; public class WindowSession { public static boolean shown; public static WindowSession get() { return new WindowSession(); } public boolean hasShownApplicationWindow() { return shown; } }',
        'com/gesher/server/Device.java': 'package com.gesher.server; public class Device { public static java.io.File packageLog(String p) { return new java.io.File(p+".log"); } public static java.io.File logsDir() { return new java.io.File("logs"); } }',
        'com/gesher/server/WindowsBluetooth.java': 'package com.gesher.server; public class WindowsBluetooth { public static int hr; public static boolean present; public static class Adapter { public int hr = WindowsBluetooth.hr; public boolean present = WindowsBluetooth.present; } public static Adapter cachedAdapter(long age) { return new Adapter(); } }',
        'com/gesher/server/Zygote.java': '''package com.gesher.server; public class Zygote {
            public static String command; public static boolean breakaway, fail;
            public static String exePath() { return "C:\\\\Gesher\\\\dalvikvm.exe"; }
            public static String windowsPath(String s) { return s; }
            public static String quote(String s) { return "\\\""+s+"\\\""; }
            public static int spawn(String s, java.util.List<String> e, String l, boolean b) {
                if(fail) throw new IllegalStateException("spawn failed"); command=s; breakaway=b; return 1;
            }}''',
    }
    with tempfile.TemporaryDirectory(prefix='gesher-crash-') as tmp:
        work = Path(tmp)
        sources = []
        for path, content in stubs.items():
            target = work / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding='utf-8')
            sources.append(str(target))
        sources += [str(ROOT / 'app/java/com/gesher/app' / p) for p in ('AppCrash.java', 'CrashDiagnosis.java')]
        sources += [str(ROOT / 'tests/app-crash/CrashDiagnosisTest.java')]
        subprocess.run(['javac', '--release', '11', '-encoding', 'UTF-8', '-d', str(work), *sources], check=True, timeout=60)
        subprocess.run(['java', '-cp', str(work), 'CrashDiagnosisTest'], cwd=work,
                       env=dict(os.environ, GESHER_CRASH_DIALOGS='1'), check=True, timeout=20)
        subprocess.run(['java', '-cp', str(work), 'CrashDiagnosisTest', 'disabled'],
                       cwd=work, env=dict(os.environ, GESHER_CRASH_DIALOGS='0'), check=True, timeout=20)
        cache = work / 'cache-test'
        cache.mkdir()
        subprocess.run(['javac', '--release', '11', '-d', str(cache),
                        str(ROOT / 'app/java/com/gesher/server/WindowsBluetooth.java'),
                        str(ROOT / 'tests/app-crash/BluetoothSnapshotTest.java')], check=True, timeout=60)
        subprocess.run(['java', '-cp', str(cache), 'BluetoothSnapshotTest'], check=True, timeout=20)


def native_test(launcher):
    from ctypes import wintypes as w
    u = ctypes.WinDLL('user32', use_last_error=True)
    callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    u.EnumWindows.argtypes = [callback, w.LPARAM]
    u.EnumChildWindows.argtypes = [w.HWND, callback, w.LPARAM]
    u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
    u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
    u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
    expected = {'generic': ('because of an error', 'עקב שגיאה'),
                'bluetooth': ('Windows currently reports no Bluetooth adapter', 'Windows מדווח כרגע שאין מתאם'),
                'security': ('security or permission check', 'בדיקת אבטחה או הרשאות'),
                'compatibility': ('could not be linked', 'לא ניתן היה לקשר')}
    for lang in ('en', 'he'):
        for kind, texts in expected.items():
            package, log = 'fixture.pkg{1}', 'C:\\תיקייה עם רווחים\\app{0}.log'
            env = dict(os.environ, GESHER_LOCALES=lang)
            p = subprocess.Popen([str(launcher), '--app-crash', package, kind, log], env=env)
            found = []
            @callback
            def visit(hwnd, unused):
                pid = w.DWORD()
                u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                name = ctypes.create_unicode_buffer(256)
                u.GetClassNameW(hwnd, name, len(name))
                if pid.value == p.pid and name.value == '#32770':
                    found.append(hwnd)
                return True
            try:
                deadline = time.monotonic() + 10
                while not found and time.monotonic() < deadline and p.poll() is None:
                    u.EnumWindows(visit, 0)
                    time.sleep(.02)
                assert found, (lang, kind, 'dialog not shown')
                hwnd = found[0]
                labels = []
                @callback
                def child(ch, unused):
                    text = ctypes.create_unicode_buffer(16384)
                    u.GetWindowTextW(ch, text, len(text))
                    labels.append(text.value)
                    return True
                u.EnumChildWindows(hwnd, child, 0)
                text = '\n'.join(labels)
                assert package in text and log in text and texts[lang == 'he'] in text, (lang, kind, text)
                # MessageBox implements MB_RTLREADING internally; Windows builds
                # differ in whether that flag appears on the child HWND styles.
                # Verify the real Hebrew text here; generated catalog tests also
                # check GesherUiRtl's language selection.
                u.PostMessageW(hwnd, 0x10, 0, 0)  # WM_CLOSE: dismiss MB_OK
                assert p.wait(timeout=5) == 0
            finally:
                if p.poll() is None:
                    p.kill()
                    p.wait()
    for args in ([], ['pkg'], ['pkg', 'unknown', 'log'], ['', 'generic', 'log']):
        assert subprocess.run([str(launcher), '--app-crash', *args], timeout=5).returncode == 1
    print('PASS: 8 actual Windows crash dialogs, literal Unicode arguments, Hebrew and malformed arguments')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--launcher', type=Path)
    args = parser.parse_args()
    host_test()
    if args.launcher:
        native_test(args.launcher.resolve())
