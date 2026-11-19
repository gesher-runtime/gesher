/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.IApplicationThread;
import android.content.pm.IPackageInstaller;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.os.Binder;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import sun.misc.Unsafe;

/** Real session admission and ownership; constructed package/process records and host JNI only. */
public final class PackageInstallerIdentityHost {
    private static Unsafe unsafe;
    private static int checks;
    private static Path data;
    private interface Call { void run() throws Exception; }
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static void rejected(Call call, String reason) throws Exception {
        try { call.run(); }
        catch (SecurityException expected) { checks++; return; }
        throw new AssertionError(reason);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }
    private static void as(int pid, int uid, Call call) throws Exception {
        long saved = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long)uid << 32) | (pid & 0xffffffffL));
            call.run();
        } finally { Binder.restoreCallingIdentity(saved); }
    }
    private static ApkPackage pkg(String name, String generation) throws Exception {
        String path = data.resolve("app/" + name + "-" + generation + "/base.apk").toString();
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, path);
        parsed.setTargetSdkVersion(36); parsed.setEnabled(true); parsed.hideAsFinal();
        ApkPackage pkg = (ApkPackage)unsafe.allocateInstance(ApkPackage.class);
        set(pkg, "packageName", name); set(pkg, "parsed", parsed); set(pkg, "apkPath", path);
        set(pkg, "mDataDir", data.resolve("data/" + name).toString());
        set(pkg, "mUid", PackageUids.uidFor(name)); set(pkg, "sharedLibraries", SharedLibraryGraph.EMPTY);
        return pkg;
    }
    @SuppressWarnings("unchecked")
    private static ActivityTaskManagerService.ProcessRecord attach(ActivityTaskManagerService atm,
            ApkPackage pkg, int pid) throws Exception {
        var result = new ActivityTaskManagerService.ProcessRecord[1];
        as(pid, pkg.uid(), () -> {
            var process = atm.new ProcessRecord(pid, new IApplicationThread.Default());
            process.packageName = pkg.packageName; process.packageCode = pkg;
            ((List<ActivityTaskManagerService.ProcessRecord>)field(atm.getClass(), "mProcesses")
                    .get(atm)).add(process);
            result[0] = process;
        });
        return result[0];
    }
    private static void run(boolean openOnly) throws Exception {
        String name = "fixture.installer.identity";
        ApkPackage original = pkg(name, "1");
        PackageManagerService pm = new PackageManagerService(List.of(original));
        ActivityTaskManagerService atm = new ActivityTaskManagerService(pm);
        var constructor = Installer.class.getDeclaredConstructor(PackageManagerService.class,
                ActivityTaskManagerService.class, Map.class);
        constructor.setAccessible(true);
        Installer backend = constructor.newInstance(pm, atm, new LinkedHashMap<>());
        pm.attachInstaller(atm, backend);
        IPackageInstaller service = pm.getPackageInstaller();
        var old = attach(atm, original, 850001);
        var params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_INHERIT_EXISTING);
        params.appPackageName = "fixture.installer.target";
        int[] session = new int[1];
        as(old.pid, old.uid, () -> {
            session[0] = service.createSession(params, name, null, 0);
            check(service.openSession(session[0]) != null, "Current installer cannot open its session");
            check(service.getSessionInfo(session[0]).getMode() == params.mode, "Inherited mode changed");
        });
        old.exiting = true;
        as(old.pid, old.uid, () -> {
            check(service.openSession(session[0]) != null, "Live exiting installer lost session authority");
            int fresh = service.createSession(params, name, null, 0);
            check(fresh != session[0], "Live exiting installer could not create another session");
            service.abandonSession(fresh);
        });
        old.dead = true;
        as(old.pid, old.uid, () -> {
            rejected(() -> service.createSession(params, name, null, 0), "Dead installer created a session");
            rejected(() -> service.openSession(session[0]), "Dead installer opened a session");
        });
        old.dead = false;
        as(old.pid, old.uid, () -> service.abandonSession(session[0]));

        // Uninstall/reinstall can complete while the old process is still exiting.
        // The replacement keeps the allocated numeric UID but has a new installed owner.
        String previousOwner = original.enabledStateOwner();
        pm.remove(name);
        ApkPackage replacement = pkg(name, "2"); pm.add(replacement);
        var current = attach(atm, replacement, 850002);
        check(original.uid() == replacement.uid()
                        && !previousOwner.equals(replacement.enabledStateOwner()),
                "Fixture did not replace installation identity while retaining its numeric UID");
        as(current.pid, current.uid, () -> session[0] = service.createSession(params, name, null, 0));
        String[] stages = stageNames();
        if (!openOnly) as(old.pid, old.uid, () -> rejected(
                () -> service.createSession(params, name, null, 0),
                "Retired installer code created a session as its replacement"));
        as(old.pid, old.uid, () -> {
            rejected(() -> service.openSession(session[0]), "Retired installer code opened a replacement session");
            rejected(() -> service.getMySessions(name, 0), "Retired installer code enumerated replacement sessions");
        });
        check(java.util.Arrays.equals(stages, stageNames()), "Rejected installer changed session storage");
        as(current.pid, current.uid, () -> {
            check(service.openSession(session[0]) != null, "Replacement lost its own session authority");
            check(service.getMySessions(name, 0).getList().size() == 1, "Replacement session inventory changed");
            service.abandonSession(session[0]);
        });
        // A different live package must still be admitted under its own identity in both UID modes.
        ApkPackage other = pkg("fixture.installer.other", "1"); pm.add(other);
        var peer = attach(atm, other, 850003);
        as(peer.pid, peer.uid, () -> {
            rejected(() -> service.createSession(params, name, null, 0), "Another package claimed the replacement name");
            int own = service.createSession(params, other.packageName, null, 0);
            check(service.openSession(own) != null, "Another current installer lost its own session authority");
            service.abandonSession(own);
            var full = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            int complete = service.createSession(full, other.packageName, null, 0);
            check(service.openSession(complete) != null, "Current installer could not create a full session");
            recompileUnchanged(backend, pm, other);
            check(service.openSession(complete) != null, "Unchanged compilation retired an existing session");
            service.abandonSession(complete);
            int afterRecompile = service.createSession(params, other.packageName, null, 0);
            check(service.openSession(afterRecompile) != null, "Unchanged compilation retired the attached caller");
            service.abandonSession(afterRecompile);
        });
        int system = service.createSession(params, "android", null, 0);
        check(service.openSession(system) != null, "Local system caller lost session authority");
        service.abandonSession(system);
    }
    private static void recompileUnchanged(Installer backend, PackageManagerService pm, ApkPackage pkg)
            throws Exception {
        String code = Path.of(pkg.apkPath).getParent().toString();
        check(!Files.exists(Dexopt.oatDir(code).toPath().resolve("base.odex")),
                "Fixture unexpectedly contains compiled code");
        Class<?> entry = Class.forName("com.gesher.server.Installer$Entry");
        var constructor = entry.getDeclaredConstructor(String.class, String.class, long.class, long.class);
        constructor.setAccessible(true);
        Object installed = constructor.newInstance(pkg.packageName, code, 1L, 1L);
        var unchanged = Installer.class.getDeclaredMethod("unchanged", String.class, String.class,
                ApkPackage.class, entry);
        unchanged.setAccessible(true);
        String[] result = (String[])unchanged.invoke(backend, pkg.apkPath, pkg.packageName, pkg, installed);
        check("unchanged".equals(result[1]) && pm.get(pkg.packageName) == pkg,
                "Compiled-code refresh changed the installed generation identity");
    }
    private static String[] stageNames() throws Exception {
        try (var files = Files.list(data.resolve("system/install-sessions"))) {
            return files.map(path -> path.getFileName().toString()).sorted().toArray(String[]::new);
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); data = Path.of(System.getenv("ANDROID_DATA")); Files.createDirectories(data);
        unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)field(PackageManager.class, "sPackageInfoCache")
                .get(null)).testPropertyName();
        run(args.length > 1 && args[1].equals("open"));
        System.out.println("PackageInstallerIdentityHost passed: " + checks + " checks; protected="
                + GmsSignatureSpoofing.isEnabled());
        System.out.println("Actual PMS/ATM/session methods; constructed manifests and process records, native identity adapter. No Windows/ART/Binder transport or APK-install execution claim.");
    }
}
