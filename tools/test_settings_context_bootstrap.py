#!/usr/bin/env python3
"""Run the production Settings context lifecycle against declared constructor seams.

ContextImpl deliberately rejects construction before service publication. Windows
SettingsBootstrapContractTest supplies the complementary actual framework run.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'app/java/android/app/GesherSettingsContext.java'
SEAMS = {
    'android/content/Context.java': '''package android.content;
public final class Context {
    public final Object thread;
    public final long identity;
    public Context(Object thread, long identity) { this.thread = thread; this.identity = identity; }
}
''',
    'android/os/Looper.java': '''package android.os;
public final class Looper {
    private static final ThreadLocal<Looper> local = new ThreadLocal<>();
    private static Looper main;
    public static Looper myLooper() { return local.get(); }
    public static Looper getMainLooper() { return main; }
    public static void prepareMainLooper() { main = new Looper(); local.set(main); }
}
''',
    'android/os/Binder.java': '''package android.os;
public final class Binder {
    private static final ThreadLocal<Long> identity = ThreadLocal.withInitial(() -> 0L);
    public static long clearCallingIdentity() { long old = identity.get(); identity.set(0L); return old; }
    public static void restoreCallingIdentity(long value) { identity.set(value); }
    public static long getCallingIdentity() { return identity.get(); }
}
''',
    'android/app/ContextImpl.java': '''package android.app;
import android.content.Context;
import android.os.Binder;
import android.os.Looper;
final class ActivityThread {
    static int constructions;
    static final Object current = new Object();
    final Looper looper = Looper.myLooper();
    ActivityThread() { if (looper == null) throw new AssertionError("ActivityThread needs a Looper"); constructions++; }
}
final class ContextImpl {
    static volatile boolean servicesReady;
    static int constructions;
    static Context createSystemContext(ActivityThread thread) {
        if (!servicesReady) throw new IllegalStateException("Display service is not published");
        constructions++;
        return new Context(thread, Binder.getCallingIdentity());
    }
}
''',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path)
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    home = args.java_home or (Path(os.environ['JAVA_HOME']) if os.environ.get('JAVA_HOME') else None)
    java = str(home / 'bin/java') if home else shutil.which('java')
    javac = str(home / 'bin/javac') if home else shutil.which('javac')
    if not java or not javac:
        parser.error('A JDK is required')
    production = (subprocess.run(['git', 'show', args.source_ref + ':' + SOURCE], cwd=ROOT,
                                check=True, capture_output=True).stdout.decode()
                  if args.source_ref else (ROOT / SOURCE).read_text())
    with tempfile.TemporaryDirectory(prefix='gesher-settings-bootstrap-') as temporary:
        work = Path(temporary)
        files = dict(SEAMS)
        files['android/app/GesherSettingsContext.java'] = production
        files['android/app/SettingsContextBootstrapHost.java'] = (
            ROOT / 'tests/settings/SettingsContextBootstrapHost.java').read_text()
        sources = []
        for relative, content in files.items():
            path = work / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            sources.append(str(path))
        subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8', '-d', str(work), *sources], check=True)
        subprocess.run([java, '-XX:-UsePerfData', '-cp', str(work), 'android.app.SettingsContextBootstrapHost'],
                       check=True, timeout=30)

if __name__ == '__main__':
    main()
