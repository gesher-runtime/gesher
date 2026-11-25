/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.IApplicationThread;
import android.app.blob.BlobHandle;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
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
    private static Object record(String name, Class<?>[] types, Object... values) throws Exception {
        var constructor = Class.forName("com.gesher.server.BlobStoreService$" + name)
                .getDeclaredConstructor(types);
        constructor.setAccessible(true);
        return constructor.newInstance(values);
    }

    /** The exact access boundary after an authenticated installation snapshot was captured. */
    private static void replacedRecipient(BlobStoreService service, PackageManagerService pm,
            ActivityTaskManagerService atm, Path root, BlobHandle handle) throws Exception {
        ApkPackage original = pkg("fixture.blob.recipient", "10");
        set(original, "mSigningIdentityChecked", true);
        set(original, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{1})});
        pm.add(original);
        var oldProcess = attach(atm, original, 860005);
        var require = BlobStoreService.class.getDeclaredMethod("requireOwner", String.class);
        require.setAccessible(true);
        Object[] captured = new Object[1];
        as(oldProcess.pid, oldProcess.uid, () -> captured[0] = require.invoke(service, original.packageName));

        pm.remove(original.packageName);
        ApkPackage replacement = pkg(original.packageName, "11");
        byte[] signer = {2};
        set(replacement, "mSigningIdentityChecked", true);
        set(replacement, "mVerifiedSignatures", new Signature[]{new Signature(signer)});
        pm.add(replacement);
        var replacementProcess = attach(atm, replacement, 860006);
        Class<?> ownerType = captured[0].getClass();
        Class<?> accessType = Class.forName("com.gesher.server.BlobStoreService$Access");
        Object grant = record("Grant", new Class<?>[]{String.class, byte[].class},
                replacement.packageName, MessageDigest.getInstance("SHA-256").digest(signer));
        Object access = record("Access", new Class<?>[]{boolean.class, boolean.class, List.class},
                false, false, List.of(grant));
        Object donor = record("Owner", new Class<?>[]{String.class, long.class, String.class},
                "android", 0L, "");
        Object contribution = record("Contribution", new Class<?>[]{ownerType, long.class, accessType},
                donor, System.currentTimeMillis(), access);
        Path bytes = root.resolve("data-recipient-fixture.bin");
        Files.write(bytes, new byte[]{42});
        Object blob = record("Blob", new Class<?>[]{long.class, BlobHandle.class, String.class,
                        long.class, long.class, Map.class, Map.class},
                99L, handle, bytes.getFileName().toString(), 1L, System.currentTimeMillis(),
                Map.of("android", contribution), Map.of());
        var permitted = BlobStoreService.class.getDeclaredMethod("access", blob.getClass(), ownerType, long.class);
        permitted.setAccessible(true);
        try {
            check(!(boolean)permitted.invoke(service, blob, captured[0], System.currentTimeMillis()),
                    "Captured old owner adopted a replacement installation's signing grant");
            as(replacementProcess.pid, replacementProcess.uid,
                    () -> captured[0] = require.invoke(service, replacement.packageName));
            check((boolean)permitted.invoke(service, blob, captured[0], System.currentTimeMillis()),
                    "Current replacement lost its own signing grant");
        } finally { Files.deleteIfExists(bytes); }
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
            // The package-admission suite verifies signed shared-UID admission.
            // At this fixture's explicit metadata boundary, model two admitted
            // peers with different installation generations and code paths.
            ApkPackage shared = pkg("fixture.blob.shared", "3");
            set(shared, "mUid", other.uid());
            pm.add(shared);
            var sharedProcess = attach(atm, shared, 860004);
            as(peer.pid, peer.uid, () -> {
                check(new CallerIdentity(pm, atm).requireCurrentPackage(shared.packageName) == shared,
                        "Shared-UID claim must retain the claimed installation, not the caller's code");
                session[0] = service.createSession(handle, shared.packageName);
                check(service.openSession(session[0], shared.packageName).getSize() == 0,
                        "Authenticated shared-UID peer cannot use the claimed package's session");
            });
            as(sharedProcess.pid, sharedProcess.uid, () -> check(
                    service.openSession(session[0], shared.packageName).getSize() == 0,
                    "Claimed package did not own the session created by its shared-UID peer"));
            peer.dead = true;
            as(peer.pid, peer.uid, () -> rejected(
                    () -> service.openSession(session[0], shared.packageName),
                    "Dead shared-UID caller acquired its live peer's session"));
            peer.dead = false;
            pm.remove(other.packageName);
            as(peer.pid, peer.uid, () -> rejected(
                    () -> service.openSession(session[0], shared.packageName),
                    "Removed shared-UID caller acquired its still-installed peer's session"));
            as(sharedProcess.pid, sharedProcess.uid,
                    () -> service.abandonSession(session[0], shared.packageName));
            replacedRecipient(service, pm, atm, root, handle);
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
        // Restore a supported legacy registry row instead of depending on the
        // private Entry constructor as installation metadata grows.
        Path registry = data.resolve("system/packages.list");
        Files.createDirectories(registry.getParent());
        Files.writeString(registry, pkg.packageName + "\t" + code + "\t1\t1\n");
        var readRegistry = Installer.class.getDeclaredMethod("readRegistry");
        readRegistry.setAccessible(true);
        Object installed = ((Map<?, ?>)readRegistry.invoke(null)).get(pkg.packageName);
        check(installed != null, "Legacy registry row did not restore the installed entry");
        Class<?> entry = installed.getClass();
        // Select the two known API shapes explicitly. An unexpected method
        // signature must still fail instead of silently skipping this check.
        Class<?> source;
        try { source = Class.forName("com.gesher.server.PackageInstallSource"); }
        catch (ClassNotFoundException baselineWithoutAttribution) { source = null; }
        var unchanged = source == null
                ? Installer.class.getDeclaredMethod("unchanged", String.class, String.class,
                        ApkPackage.class, entry)
                : Installer.class.getDeclaredMethod("unchanged", String.class, String.class,
                        ApkPackage.class, entry, source);
        unchanged.setAccessible(true);
        String[] result = (String[])(source == null
                ? unchanged.invoke(installer, pkg.apkPath, pkg.packageName, pkg, installed)
                : unchanged.invoke(installer, pkg.apkPath, pkg.packageName, pkg, installed, null));
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
