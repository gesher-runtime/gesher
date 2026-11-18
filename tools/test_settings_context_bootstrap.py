#!/usr/bin/env python3
"""Run the production Settings context lifecycle against declared constructor seams.

ContextImpl rejects construction before feature-cache readiness and service
publication. The package-manager, cache and checked-error seams expose inputs and
calls of the actual helper; they are not Android service implementations. Windows
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
    'android/os/RemoteException.java': '''package android.os;
public final class RemoteException extends Exception {
    public final RuntimeException translated;
    public int translations;
    public RemoteException(String message) {
        super(message);
        translated = new RuntimeException("From system server", this);
    }
    public RuntimeException rethrowFromSystemServer() { translations++; return translated; }
}
''',
    'android/content/pm/FeatureInfo.java': '''package android.content.pm;
public final class FeatureInfo {
    public String name;
    public int version;
}
''',
    'android/content/pm/ParceledListSlice.java': '''package android.content.pm;
import java.util.List;
public final class ParceledListSlice<T> {
    private final List<T> list;
    public ParceledListSlice(List<T> list) { this.list = list; }
    public List<T> getList() { return list; }
}
''',
    'android/content/pm/IPackageManager.java': '''package android.content.pm;
import android.os.RemoteException;
public interface IPackageManager {
    ParceledListSlice<FeatureInfo> getSystemAvailableFeatures() throws RemoteException;
}
''',
    'android/content/pm/SystemFeaturesCache.java': '''package android.content.pm;
import android.os.Binder;
import java.util.List;
public final class SystemFeaturesCache {
    private static volatile SystemFeaturesCache instance;
    public static int constructions;
    public static int publications;
    public static long publicationIdentity = -1L;
    public final List<FeatureInfo> receivedFeatures;
    public final long constructionIdentity;
    public SystemFeaturesCache(List<FeatureInfo> features) {
        receivedFeatures = features;
        constructionIdentity = Binder.getCallingIdentity();
        constructions++;
    }
    public static boolean hasInstance() { return instance != null; }
    public static void setInstance(SystemFeaturesCache cache) {
        instance = cache;
        publicationIdentity = Binder.getCallingIdentity();
        publications++;
    }
    public static SystemFeaturesCache fixtureInstance() { return instance; }
}
''',
    'android/app/ContextImpl.java': '''package android.app;
import android.content.Context;
import android.content.pm.IPackageManager;
import android.content.pm.SystemFeaturesCache;
import android.os.Binder;
import android.os.Looper;
final class ActivityThread {
    static int constructions;
    static final Object current = new Object();
    static IPackageManager packageManager;
    static int packageManagerLookups;
    static long packageManagerLookupIdentity = -1L;
    final Looper looper = Looper.myLooper();
    ActivityThread() { if (looper == null) throw new AssertionError("ActivityThread needs a Looper"); constructions++; }
    static IPackageManager getPackageManager() {
        packageManagerLookups++;
        packageManagerLookupIdentity = Binder.getCallingIdentity();
        return packageManager;
    }
}
final class ContextImpl {
    static volatile boolean servicesReady;
    static int attempts;
    static int constructions;
    static long attemptIdentity = -1L;
    static Context createSystemContext(ActivityThread thread) {
        attempts++;
        attemptIdentity = Binder.getCallingIdentity();
        if (!SystemFeaturesCache.hasInstance()) throw new AssertionError("Feature cache is not initialized");
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
        for mode in ('initialize', 'retained'):
            subprocess.run([java, '-XX:-UsePerfData', '-cp', str(work),
                            'android.app.SettingsContextBootstrapHost', mode], check=True, timeout=30)

if __name__ == '__main__':
    main()
