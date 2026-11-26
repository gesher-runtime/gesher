#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Execute AppShell's real publication/cleanup methods at explicit host boundaries.

Extracted method bodies are unchanged. APK parsing/icon rendering, Android Parcel
and Windows shell calls are isolated fixtures; profile property files use real I/O.
This checks suppression and local state, not Win32 integration or Android dispatch.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_install_metadata_comparison import method

ROOT = Path(__file__).resolve().parents[1]
METHODS = (
    'private static boolean portableMode(', 'public static String appUserModelId(',
    'private static File shellDir(', 'public static Properties shortcutOf(',
    'public static void registerQuietly(', 'private static void register(',
    'private static void writePortableRecord(', 'private static void writeLauncherRecord(',
    'private static void removeShortcut(', 'private static void dynamicPackage(',
    'public static String dynamicKey(', 'private static File dynamicFile(',
    'public static synchronized Properties dynamicShortcutsOf(',
    'private static boolean removeDynamicLink(', 'private static boolean sameDynamicLink(',
    'public static synchronized boolean syncDynamicShortcuts(',
    'public static synchronized void removeAllDynamicShortcuts(',
    'public static Properties urlProtocolsOf(', 'public static void registerUrlProtocolsQuietly(',
    'private static void registerUrlProtocols(', 'public static void unregisterUrlProtocolsQuietly(',
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path,
                        default=Path(os.environ['JAVA_HOME']) if os.environ.get('JAVA_HOME') else None)
    args = parser.parse_args()
    source = (ROOT / 'app/java/com/gesher/app/shell/AppShell.java').read_text(encoding='utf-8')
    exact = '\n'.join(method(source, signature) for signature in METHODS)
    template = (ROOT / 'tests/portable/PortableShellHost.java.in').read_text(encoding='utf-8')
    stubs = {
        'android/content/Intent.java': 'package android.content; public class Intent {}',
        'android/content/pm/ActivityInfo.java': 'package android.content.pm; public class ActivityInfo {}',
        'android/content/pm/ShortcutInfo.java': '''package android.content.pm;
            public class ShortcutInfo {
              public String getId() { return "id"; }
              public android.content.Intent[] getIntents() { return new android.content.Intent[0]; }
              public void writeToParcel(android.os.Parcel p, int flags) { }
              public boolean isEnabled() { return true; }
              public CharSequence getShortLabel() { return "Shortcut"; }
              public CharSequence getLongLabel() { return null; }
            }''',
        'android/os/Parcel.java': '''package android.os; public class Parcel {
            public static Parcel obtain() { return new Parcel(); }
            public boolean hasFileDescriptors() { return false; }
            public byte[] marshall() { return new byte[]{1,2,3}; }
            public void recycle() {}
        }''',
        'com/gesher/server/ApkPackage.java': '''package com.gesher.server; public class ApkPackage {
            public String apkPath = "fixture.apk";
            public static ApkPackage parseCodeDirectory(String path) { return new ApkPackage(); }
            public Object applicationInfo(int flags) { return null; }
        }''',
        'com/gesher/server/MicrogAddons.java': '''package com.gesher.server; public class MicrogAddons {
            public static ApkPackage stagedAddon(String pkg, String path) { return null; }
        }''',
    }
    with tempfile.TemporaryDirectory(prefix='gesher-portable-shell-') as temporary:
        work = Path(temporary)
        files = []
        for name, text in stubs.items():
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8'); files.append(str(path))
        generated = work / 'PortableShellHost.java'
        generated.write_text(template.replace('@PRODUCTION_METHODS@', exact), encoding='utf-8')
        javac = str(args.java_home / 'bin/javac') if args.java_home else 'javac'
        java = str(args.java_home / 'bin/java') if args.java_home else 'java'
        subprocess.run([javac, '-encoding', 'UTF-8', '-d', str(work), str(generated), *files],
                       check=True, timeout=60)
        for mode in ('1', '0'):
            env = dict(os.environ, GESHER_PORTABLE=mode, GESHER_SHORTCUTS='1',
                       ANDROID_DATA=str(work / ('data-' + mode)))
            subprocess.run([java, '-cp', str(work), 'PortableShellHost'],
                           env=env, check=True, timeout=30)


if __name__ == '__main__':
    main()
