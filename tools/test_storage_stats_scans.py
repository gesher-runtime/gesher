#!/usr/bin/env python3
"""Production storage scans with real files and counted traversal boundaries.

Android Binder/package metadata are host stubs. Files.walkFileTree delegates to
the real filesystem and unmodified production visitor. --source-ref runs the
same benchmark on older service source; --report-only omits its performance gate.
Host work counts do not measure Windows/ART Binder or physical-disk latency.
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    'android/Manifest.java': 'package android; public class Manifest { public static class permission { public static final String PACKAGE_USAGE_STATS="usage"; } }',
    'android/os/Binder.java': '''package android.os; public class Binder {
      private static final ThreadLocal<Integer> UID=ThreadLocal.withInitial(() -> 1000);
      public static int getCallingUid(){return UID.get();}
      public static void setCallingUid(int uid){UID.set(uid);}
    }''',
    'android/os/Process.java': '''package android.os; public class Process {
      public static final int SYSTEM_UID=1000,ROOT_UID=0,SHELL_UID=2000;
      public static int myUid(){return SYSTEM_UID;} }''',
    'android/os/Environment.java': 'package android.os; public class Environment { public static java.io.File getDataDirectory(){return new java.io.File(".");} }',
    'android/os/ParcelableException.java': 'package android.os; public class ParcelableException extends RuntimeException { public ParcelableException(Throwable cause){super(cause);} }',
    'android/os/storage/StorageManager.java': '''package android.os.storage; public class StorageManager {
      public static final String UUID_PRIMARY_PHYSICAL="primary_physical";
      public static final java.util.UUID UUID_DEFAULT=new java.util.UUID(0,0); }''',
    'android/content/pm/PackageManager.java': '''package android.content.pm; public class PackageManager {
      public static class NameNotFoundException extends Exception { public NameNotFoundException(String n){super(n);} } }''',
    'android/content/pm/ParceledListSlice.java': 'package android.content.pm; public class ParceledListSlice { public static ParceledListSlice emptyList(){return new ParceledListSlice();} }',
    'android/app/usage/StorageStats.java': 'package android.app.usage; public class StorageStats { public long codeBytes,apkBytes,dataBytes,cacheBytes,externalCacheBytes; }',
    'android/app/usage/ExternalStorageStats.java': 'package android.app.usage; public class ExternalStorageStats { public long totalBytes,audioBytes,videoBytes,imageBytes,appBytes,obbBytes; }',
    'com/gesher/server/ApkPackage.java': '''package com.gesher.server; final class ApkPackage {
      final String packageName,apkPath; final int uid; volatile String ce,de; final Parsed parsed=new Parsed();
      ApkPackage(String n,String a,int u,String c,String d){packageName=n;apkPath=a;uid=u;ce=c;de=d;}
      int uid(){return uid;} String credentialProtectedDataDir(){return ce;} String deviceProtectedDataDir(){return de;}
      static final class Parsed { volatile java.util.List<String> permissions=java.util.Collections.emptyList();
        java.util.List<String> getRequestedPermissions(){return permissions;} } }''',
    'com/gesher/server/PackageManagerService.java': '''package com.gesher.server; final class PackageManagerService {
      private volatile java.util.Map<String,ApkPackage> packages=new java.util.LinkedHashMap<>();
      final java.util.Set<String> removing=java.util.concurrent.ConcurrentHashMap.newKeySet();
      synchronized void put(ApkPackage p){java.util.Map<String,ApkPackage> m=new java.util.LinkedHashMap<>(packages);m.put(p.packageName,p);packages=m;}
      ApkPackage get(String n){return packages.get(n);} boolean isRemovalPending(String n){return removing.contains(n);}
      java.util.Collection<ApkPackage> packages(){return packages.values();}
      String[] getPackagesForUid(int uid){return packages.values().stream().filter(p->p.uid()==uid).map(p->p.packageName).toArray(String[]::new);} }''',
}


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--java-home', type=Path, required=True)
    p.add_argument('--source-ref')
    p.add_argument('--benchmark', action='store_true')
    p.add_argument('--report-only', action='store_true')
    args = p.parse_args()
    if args.report_only and not args.benchmark:
        p.error('--report-only requires --benchmark')
    with tempfile.TemporaryDirectory(prefix='gesher-storage-scans-') as directory:
        work = Path(directory)
        sources = []

        def write(name: str, text: str) -> None:
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
            sources.append(str(path))

        for name, text in STUBS.items():
            write(name, text)
        for name in ('StorageStatsService', 'StorageStatsScans'):
            relative = f'app/java/com/gesher/server/{name}.java'
            if args.source_ref and name == 'StorageStatsService':
                source = subprocess.run(['git', 'show', args.source_ref + ':' + relative], cwd=ROOT,
                                        check=True, capture_output=True, text=True).stdout
            else:
                source = (ROOT / relative).read_text(encoding='utf-8')
            print(name + ' SHA256 ' + hashlib.sha256(source.encode()).hexdigest(), flush=True)
            if name == 'StorageStatsService':
                # Only construct interface signatures; no production body is replaced.
                methods = re.findall(r'@Override\s+public (boolean|long|StorageStats|ExternalStorageStats|ParceledListSlice)\s+(\w+\([^)]*\))\s*\{', source)
                methods = [(t, s) for t, s in methods if not s.startswith('equals(')]
                if len(methods) != 13:
                    raise ValueError('Storage Binder interface changed')
                declarations = '\n'.join('public ' + t + ' ' + s + ' {return '
                                         + ('false' if t == 'boolean' else '0' if t == 'long' else 'null')
                                         + ';}' for t, s in methods)
                write('android/app/usage/IStorageStatsManager.java',
                      'package android.app.usage; import android.content.pm.ParceledListSlice; '
                      'public interface IStorageStatsManager { class Default {' + declarations + '}}')
                if source.count('Files.walkFileTree(') != 1:
                    raise ValueError('Expected one filesystem traversal boundary')
                source = source.replace('Files.walkFileTree(', 'CountingFiles.walkFileTree(')
            write('com/gesher/server/' + name + '.java', source)
        sources += [str(ROOT / 'tests/storage-stats/CountingFiles.java'),
                    str(ROOT / 'tests/storage-stats/StorageStatsScansHost.java')]
        classes = work / 'classes'
        classes.mkdir()
        subprocess.run([str(args.java_home / 'bin/javac'), '-encoding', 'UTF-8', '-d', str(classes), *sources],
                       check=True, timeout=90)
        command = [str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx256m',
                   '-cp', str(classes), 'com.gesher.server.StorageStatsScansHost']
        if args.benchmark:
            command.append('--benchmark')
        if args.report_only:
            command.append('--report-only')
        subprocess.run(command, check=True, timeout=180)


if __name__ == '__main__':
    main()
