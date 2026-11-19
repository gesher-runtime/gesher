/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.IApplicationThread;
import android.app.blob.BlobHandle;
import android.content.pm.PackageManager;
import android.os.Binder;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.List;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import sun.misc.Unsafe;

/** Actual BlobStore/PMS/ATM admission with explicit manifest, process and Binder boundaries. */
public final class BlobStoreIdentityHost {
    private static Unsafe unsafe;
    private static Path data;
    private static int checks;
    private interface Call { void run() throws Exception; }
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static void rejected(Call operation, String reason) throws Exception {
        try { operation.run(); }
        catch (SecurityException expected) { checks++; return; }
        throw new AssertionError(reason);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }
    private static void as(int pid, int uid, Call operation) throws Exception {
        long previous = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long)uid << 32) | (pid & 0xffffffffL));
            operation.run();
        } finally { Binder.restoreCallingIdentity(previous); }
    }
    private static ApkPackage pkg(String name, String generation) throws Exception {
        String path = data.resolve("app/" + name + "-" + generation + "/base.apk").toString();
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, path);
        parsed.setTargetSdkVersion(36); parsed.setEnabled(true); parsed.hideAsFinal();
        ApkPackage pkg = (ApkPackage)unsafe.allocateInstance(ApkPackage.class);
        set(pkg, "packageName", name); set(pkg, "parsed", parsed); set(pkg, "apkPath", path);
        set(pkg, "mDataDir", data.resolve("data/" + name).toString());
        set(pkg, "mUid", PackageUids.uidFor(name)); set(pkg, "sharedLibraries", SharedLibraryGraph.EMPTY);
        pkg.setInstallTimes(Long.parseLong(generation), Long.parseLong(generation));
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
    private static String[] stageNames(Path root) throws Exception {
        try (var files = Files.list(root)) {
            return files.map(path -> path.getFileName().toString()).sorted().toArray(String[]::new);
        }
    }
    private static void run(boolean openOnly) throws Exception {
        String name = "fixture.blob.identity";
        ApkPackage original = pkg(name, "1");
        PackageManagerService pm = new PackageManagerService(List.of(original));
        ActivityTaskManagerService atm = new ActivityTaskManagerService(pm);
        var constructor = Installer.class.getDeclaredConstructor(PackageManagerService.class,
                ActivityTaskManagerService.class, Map.class);
        constructor.setAccessible(true);
        Installer installer = constructor.newInstance(pm, atm, new LinkedHashMap<>());
        Path root = data.resolve("system/blobs");
        BlobStoreService service = new BlobStoreService(pm, new CallerIdentity(pm, atm), root.toFile());
        ScheduledThreadPoolExecutor worker = (ScheduledThreadPoolExecutor)field(
                BlobStoreService.class, "mWorker").get(service);
        try {
            var old = attach(atm, original, 860001);
            BlobHandle handle = BlobHandle.createWithSha256(
                    MessageDigest.getInstance("SHA-256").digest(new byte[]{42}), "identity", 0, "fixture");
            long[] session = new long[1];
            as(old.pid, old.uid, () -> {
                session[0] = service.createSession(handle, name);
                check(service.openSession(session[0], name).getSize() == 0,
                        "Current caller lost its own blob session");
            });
            worker.submit(() -> {}).get(10, TimeUnit.SECONDS);
            old.exiting = true;
            as(old.pid, old.uid, () -> {
                check(service.openSession(session[0], name).getSize() == 0,
                        "Live exiting caller lost its current blob session");
                long fresh = service.createSession(handle, name);
                check(fresh != session[0], "Current exiting caller cannot create a blob session");
                service.abandonSession(fresh, name);
            });
            old.dead = true;
            as(old.pid, old.uid, () -> {
                rejected(() -> service.createSession(handle, name), "Dead caller created a blob session");
                rejected(() -> service.openSession(session[0], name), "Dead caller opened a blob session");
            });
            old.dead = false;
            as(old.pid, old.uid, () -> service.abandonSession(session[0], name));
            pm.remove(name);
            ApkPackage replacement = pkg(name, "2"); pm.add(replacement);
            var current = attach(atm, replacement, 860002);
            check(original.uid() == replacement.uid(), "Replacement did not retain its numeric UID");
            as(current.pid, current.uid, () -> session[0] = service.createSession(handle, name));
            String[] before = stageNames(root);
            if (!openOnly) as(old.pid, old.uid, () -> rejected(
                    () -> service.createSession(handle, name),
                    "Retired caller created a blob session as the replacement"));
            as(old.pid, old.uid, () -> {
                rejected(() -> service.openSession(session[0], name),
                        "Retired caller opened the replacement's blob session");
                rejected(() -> service.getLeasedBlobs(name), "Retired caller queried replacement leases");
            });
            check(java.util.Arrays.equals(before, stageNames(root)), "Rejected caller mutated blob storage");
            as(current.pid, current.uid, () -> {
                check(service.openSession(session[0], name).getSize() == 0,
                        "Replacement lost its own blob session");
                service.abandonSession(session[0], name);
            });
            ApkPackage other = pkg("fixture.blob.other", "1"); pm.add(other);
            var peer = attach(atm, other, 860003);
            as(peer.pid, peer.uid, () -> {
                rejected(() -> service.createSession(handle, name), "Other caller claimed the replacement");
                long own = service.createSession(handle, other.packageName);
                check(service.openSession(own, other.packageName).getSize() == 0,
                        "Other current caller lost its blob session");
                recompileUnchanged(installer, pm, other);
                check(service.openSession(own, other.packageName).getSize() == 0,
                        "Cache-only compilation retired a current blob session");
                service.abandonSession(own, other.packageName);
                long fresh = service.createSession(handle, other.packageName);
                check(service.openSession(fresh, other.packageName).getSize() == 0,
                        "Cache-only compilation retired the attached caller");
                service.abandonSession(fresh, other.packageName);
            });
            long system = service.createSession(handle, "android");
            check(service.openSession(system, "android").getSize() == 0, "System blob session rejected");
            service.abandonSession(system, "android");
        } finally {
            worker.shutdownNow();
            check(worker.awaitTermination(10, TimeUnit.SECONDS), "Blob worker failed to terminate");
        }
    }
    private static void recompileUnchanged(Installer installer, PackageManagerService pm, ApkPackage pkg)
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
        String[] result = (String[])unchanged.invoke(installer, pkg.apkPath, pkg.packageName, pkg, installed);
        check("unchanged".equals(result[1]) && pm.get(pkg.packageName) == pkg,
                "Compiled-code refresh changed the installed generation identity");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); data = Path.of(System.getenv("ANDROID_DATA")); Files.createDirectories(data);
        unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)field(PackageManager.class, "sPackageInfoCache")
                .get(null)).testPropertyName();
        run(args.length > 1 && args[1].equals("open"));
        System.out.println("BlobStoreIdentityHost passed: " + checks + " checks; protected="
                + GmsSignatureSpoofing.isEnabled());
        System.out.println("Actual service methods and files; constructed manifest/process records and JNI identity. No Windows/ART or remote Binder claim.");
    }
}
