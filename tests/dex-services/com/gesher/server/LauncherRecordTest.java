/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import com.gesher.app.shell.AppShell;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;

/**
 * The Control Center's no-launcher-activity record (AppShell) on ART, with the real
 * PackageManagerService over installed code generations in a data directory of its own:
 *
 *   LauncherRecordTest <APK with a MAIN/LAUNCHER activity> <APK without one>
 *
 * (tests/apps' lifecycle.apk and gesher-camera.apk). Covers the marker written on trusted
 * package publication even with GESHER_SHORTCUTS=0, the system server's listener after
 * setComponentEnabledSetting / setApplicationEnabledSetting (called from the package event
 * executor, outside PackageManagerService's monitor), the refresh over installed packages at the
 * device's start, and that neither brings a removed package's data directory back.
 */
public final class LauncherRecordTest {
    private static final String MARKER = "gesher-shell/no-launcher-activity";
    private static int checks;
    private static File data;

    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    // Boot class path members that are package-private: reflection, as across class loaders.
    private static Object call(Object target, String name, Class<?>[] signature, Object... args)
            throws Exception {
        Class<?> type = target instanceof Class<?> c ? c : target.getClass();
        java.lang.reflect.Method m = type.getDeclaredMethod(name, signature);
        m.setAccessible(true);
        try {
            return m.invoke(target instanceof Class<?> ? null : target, args);
        } catch (java.lang.reflect.InvocationTargetException e) {
            if (e.getCause() instanceof Exception x) throw x;
            throw (Error) e.getCause();
        }
    }

    /** The system server's production wiring (SystemServer.trackLauncherRecords). */
    private static Thread track(PackageManagerService pm) throws Exception {
        return (Thread) call(SystemServer.class, "trackLauncherRecords",
                new Class<?>[]{PackageManagerService.class}, pm);
    }

    private static File marker(String pkg) { return new File(data, "data/" + pkg + "/" + MARKER); }

    private static boolean recorded(String pkg) { return marker(pkg).isFile(); }

    /** Installs {@code apk} as the installer lays a code generation out: app/<package>-1/base.apk. */
    private static ApkPackage install(String apk) throws Exception {
        String name = ApkPackage.parse(apk).packageName;
        File code = new File(data, "app/" + name + "-1").getCanonicalFile();
        check(code.mkdirs(), "code directory of " + name);
        Files.copy(new File(apk).toPath(), new File(code, "base.apk").toPath(),
                StandardCopyOption.REPLACE_EXISTING);
        // The package's data directory, as its first launch makes it.
        check(new File(data, "data/" + name).mkdirs(), "data directory of " + name);
        return ApkPackage.parseCodeDirectory(code.getPath());
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("usage: LauncherRecordTest LAUNCHER.apk NOLAUNCHER.apk");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        // A private writable cache region (package info cache invalidation), as
        // ServiceFixtureRunner gives in-process server fixtures.
        Class<?> shared = Class.forName("com.android.internal.os.ApplicationSharedMemory");
        shared.getMethod("setInstance", shared).invoke(null, shared.getMethod("create").invoke(null));
        File base = new File(System.getenv("ANDROID_DATA")).getParentFile();
        data = new File(base, "launcher-record-" + android.os.Process.myPid()).getCanonicalFile();
        deleteTree(data);
        check(new File(data, "system").mkdirs(), "temporary data directory");
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        check(Device.dataDir().equals(data.getAbsolutePath()), "Device.dataDir follows ANDROID_DATA");
        // No Start menu shortcuts: the record is kept all the same.
        android.system.Os.setenv(AppShell.ENV_SHORTCUTS, "0", true);
        try {
            run(args[0], args[1]);
        } finally {
            deleteTree(data);
        }
        System.out.println("LauncherRecordTest all passed: " + checks + " checks");
    }

    @SuppressWarnings("unchecked")
    private static void run(String launcherApk, String plainApk) throws Exception {
        ApkPackage launcher = install(launcherApk), plain = install(plainApk);
        String withLauncher = launcher.packageName, without = plain.packageName;
        ComponentName launcherActivity = new ComponentName(withLauncher,
                launcher.launcherActivity(null).name);

        // The real package manager; package events queued, then run on this thread, outside its
        // monitor, as its executor does.
        Constructor<PackageManagerService> constructor = PackageManagerService.class
                .getDeclaredConstructor(java.util.Collection.class, boolean.class);
        constructor.setAccessible(true);
        PackageManagerService pm = constructor.newInstance(List.of(launcher, plain), false);
        ArrayDeque<Runnable> events = new ArrayDeque<>();
        field(PackageManagerService.class, "mPackageEventExecutor").set(pm, (Executor) events::add);
        field(PackageManagerService.class, "mPackageEventSender").set(pm, (Consumer<?>) intent -> { });

        // 2. The refresh at the device's start, over packages recorded wrongly (or never).
        marker(without).delete();
        marker(withLauncher).getParentFile().mkdirs();
        Files.write(marker(withLauncher).toPath(), new byte[0]);
        Thread refresh = track(pm);
        refresh.join(30000);
        check(!refresh.isAlive(), "refresh finished");
        check(recorded(without) && !recorded(withLauncher), "the refresh corrects every installed package's record");

        // Package publication, not a shortcut's raw APK reader, owns the record.
        marker(without).delete();
        call(pm, "packageInstalled", new Class<?>[]{ApkPackage.class, boolean.class, Object.class}, plain, false, pm);
        while (!events.isEmpty()) events.poll().run();
        check(recorded(without), "a package without a launcher activity is recorded on publication with shortcuts off");
        check(!recorded(withLauncher) && AppShell.shortcutOf(withLauncher) == null,
                "a launchable package has no marker or shortcut with shortcuts off");

        // The production listener, checked to run outside the package manager's monitor.
        Field listenerField = field(PackageManagerService.class, "mDesktopEnabledStateListener");
        Consumer<ApkPackage> production = (Consumer<ApkPackage>) listenerField.get(pm);
        check(production != null, "the system server set the desktop listener");
        List<String> calls = new ArrayList<>();
        listenerField.set(pm, (Consumer<ApkPackage>) p -> {
            check(!Thread.holdsLock(pm), "desktop listener ran under PackageManagerService's monitor");
            calls.add(p.packageName);
            production.accept(p);
        });

        // 3. setComponentEnabledSetting of the launcher activity: recorded, then not.
        pm.setComponentEnabledSetting(launcherActivity, PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                PackageManager.DONT_KILL_APP, 0, "android");
        check(!recorded(withLauncher), "nothing happens before the package event runs");
        while (!events.isEmpty()) events.poll().run();
        check(calls.equals(List.of(withLauncher)), "the listener saw the change: " + calls);
        check(recorded(withLauncher), "disabling the only launcher activity records the package");
        AppShell.registerQuietly(withLauncher, new File(launcher.apkPath).getParent(), true);
        check(recorded(withLauncher), "shortcut registration cannot overwrite the installed disabled state");
        pm.setComponentEnabledSetting(launcherActivity, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                PackageManager.DONT_KILL_APP, 0, "android");
        while (!events.isEmpty()) events.poll().run();
        check(!recorded(withLauncher), "enabling it again removes the record");

        // setApplicationEnabledSetting: a disabled application has no launcher activity.
        pm.setApplicationEnabledSetting(withLauncher, PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                PackageManager.DONT_KILL_APP, 0, "android");
        while (!events.isEmpty()) events.poll().run();
        check(recorded(withLauncher), "a disabled application is recorded");
        pm.setApplicationEnabledSetting(withLauncher, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                PackageManager.DONT_KILL_APP, 0, "android");
        while (!events.isEmpty()) events.poll().run();
        check(!recorded(withLauncher), "an enabled application with a launcher activity is not");

        // 4. A removed package's data directory is never brought back by a record update.
        File removedData = new File(data, "data/" + without);
        deleteTree(removedData);
        AppShell.updateLauncherRecordQuietly(without, plain);
        check(!removedData.exists(), "a record update does not recreate a removed package's data directory");
        check(new File(data, "data/" + without).mkdirs(), "data directory again");

        // 5. The refresh racing removals and re-additions: no failure, and once quiet, every
        // installed package's record is right.
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread.UncaughtExceptionHandler old = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> failure.compareAndSet(null, e));
        try {
            for (int i = 0; i < 20; i++) {
                Thread racing = track(pm);
                call(pm, "remove", new Class<?>[]{String.class}, without);
                while (!events.isEmpty()) events.poll().run();
                ApkPackage again = ApkPackage.parseCodeDirectory(new File(plain.apkPath).getParent());
                call(pm, "add", new Class<?>[]{ApkPackage.class}, again);
                while (!events.isEmpty()) events.poll().run();
                racing.join(30000);
                check(!racing.isAlive(), "racing refresh finished");
            }
        } finally {
            Thread.setDefaultUncaughtExceptionHandler(old);
        }
        check(failure.get() == null, "refresh failed while packages changed: " + failure.get());
        marker(without).delete();
        Thread quiet = track(pm);
        quiet.join(30000);
        check(recorded(without) && !recorded(withLauncher), "records are right once packages are quiet");
        AppShell.unregisterQuietly(without, null);
        check(!marker(without).getParentFile().exists(), "unregister removes an owned launcher marker and empty shell directory");
    }
}
