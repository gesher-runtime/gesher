/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.IApplicationThread;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.Signature;
import android.os.Binder;
import android.os.IBinder;
import com.android.internal.app.IAppOpsCallback;
import com.gesher.server.ApkPackage;
import com.gesher.server.ExternalStorageAccess;
import com.gesher.server.PackageManagerService;
import com.gesher.server.Services;
import java.io.File;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;
import sun.misc.Unsafe;

/** Real storage policy, AppOps, lifecycle publication and mount-service contracts. */
public final class ExternalStoragePolicyContractTest {
    private static final String NAME = "fixture.storage.contract", PEER = "fixture.storage.foreign";
    private static final int UID = 12991, PID = 899910;
    private static final int OP = AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE;
    private static final String IMAGES = Manifest.permission.READ_MEDIA_IMAGES;
    private static final String AUDIO = Manifest.permission.READ_MEDIA_AUDIO;
    private static final String READ = Manifest.permission.READ_EXTERNAL_STORAGE;
    private static final String WRITE = Manifest.permission.WRITE_EXTERNAL_STORAGE;
    private static int checks;
    private interface Call { void run() throws Exception; }
    private interface BeforeWrite {
        void run(ApkPackage pkg, ExternalStorageAccess.Policy policy, File target) throws Exception;
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Field field(Class<?> owner, String name) throws Exception {
        Field field = owner.getDeclaredField(name); field.setAccessible(true); return field;
    }
    private static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }
    private static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    private static Object make(Class<?> owner, Class<?>[] parameters, Object... args) throws Exception {
        var constructor = owner.getDeclaredConstructor(parameters); constructor.setAccessible(true);
        return constructor.newInstance(args);
    }
    private static Object call(Object target, String name, Class<?>[] parameters, Object... args) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name, parameters); method.setAccessible(true);
        return method.invoke(target, args);
    }
    private static void rejects(Class<? extends Throwable> expected, Call action, String message) throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            while ((failure instanceof InvocationTargetException || failure instanceof java.util.concurrent.ExecutionException)
                    && failure.getCause() != null) failure = failure.getCause();
            if (expected.isInstance(failure)) { checks++; return; }
            throw new AssertionError(message + ": " + failure, failure);
        }
        throw new AssertionError(message);
    }
    private static void await(BooleanSupplier condition, String message) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) Thread.sleep(2);
        check(condition.getAsBoolean(), message);
    }
    private static void waitFor(CountDownLatch latch) throws IOException {
        try {
            if (!latch.await(10, TimeUnit.SECONDS)) throw new IOException("Timed out at isolated file-I/O boundary");
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt(); throw new IOException(interrupted);
        }
    }
    private static FutureTask<Void> worker(String name, Call action) {
        FutureTask<Void> task = new FutureTask<>(() -> { action.run(); return null; });
        Thread thread = new Thread(task, name); thread.setDaemon(true); thread.start(); return task;
    }
    private static ApkPackage pkg(String name, int uid) throws Exception {
        return pkg(name, uid, 34);
    }
    private static ApkPackage pkg(String name, int uid, int targetSdk) throws Exception {
        Unsafe unsafe = (Unsafe) field(Unsafe.class, "theUnsafe").get(null);
        Method make = ExternalStorageAccessTest.class.getDeclaredMethod("makePackage",
                Unsafe.class, String.class, int.class, int.class, boolean.class, String[].class);
        make.setAccessible(true);
        ApkPackage pkg = (ApkPackage) make.invoke(null, unsafe, name, uid, targetSdk, false,
                new String[] {Manifest.permission.MANAGE_EXTERNAL_STORAGE, IMAGES, AUDIO,
                        READ, WRITE, Manifest.permission.CAMERA});
        set(pkg, "mSigningIdentityChecked", true);
        set(pkg, "mVerifiedSignatures", new Signature[] {new Signature(new byte[] {7})});
        return pkg;
    }
    private static final class Harness implements AutoCloseable {
        final ApkPackage owner, peer;
        final PackageManagerService pm;
        final Object atm, am, process;
        final AtomicInteger exits = new AtomicInteger();
        final FixtureServiceContext.Scope contexts = new FixtureServiceContext.Scope();
        Harness(boolean host) throws Exception { this(host, 34); }
        @SuppressWarnings("unchecked") Harness(boolean host, int targetSdk) throws Exception {
            owner = pkg(NAME, UID, targetSdk); peer = pkg(PEER, UID + 1, targetSdk);
            pm = (PackageManagerService) make(PackageManagerService.class,
                    new Class<?>[] {Collection.class, boolean.class}, List.of(owner, peer), false);
            if (host) {
                // Only AssetManager's native metadata reader is unavailable on the host.
                // Real PermissionPolicy still owns all runtime decisions and AppOps flags.
                Object catalog = get(pm.permissions(), "mCatalog");
                Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>) get(catalog, "mFramework");
                for (String name : List.of(IMAGES, AUDIO, READ, WRITE, Manifest.permission.CAMERA)) {
                    PermissionInfo info = new PermissionInfo(name.equals(Manifest.permission.CAMERA)
                            ? Manifest.permission.BACKGROUND_CAMERA : null);
                    info.name = name; info.packageName = "android";
                    info.protectionLevel = PermissionInfo.PROTECTION_DANGEROUS;
                    if (name.equals(READ) || name.equals(WRITE)) info.flags |= PermissionInfo.FLAG_SOFT_RESTRICTED;
                    definitions.put(name, info);
                }
                set(catalog, "mLoaded", true);
                call(pm.permissions(), "initializePackages", new Class<?>[0]);
            }
            set(pm, "mPackageEventSender", (java.util.function.Consumer<android.content.Intent>) event -> { });
            atm = make(type("ActivityTaskManagerService"), new Class<?>[] {PackageManagerService.class}, pm);
            am = make(type("ActivityManagerService"), new Class<?>[] {atm.getClass(), PackageManagerService.class,
                    type("NotificationManagerService")}, atm, pm, null);
            field(Services.class, "sLast").set(null, contexts.runtime(pm, atm, am));
            IApplicationThread thread = new IApplicationThread.Default() {
                final Binder token = new Binder();
                @Override public IBinder asBinder() { return token; }
                @Override public void scheduleExit() { exits.incrementAndGet(); }
            };
            long identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) UID << 32) | PID);
                process = make(type("ActivityTaskManagerService$ProcessRecord"),
                        new Class<?>[] {atm.getClass(), int.class, IApplicationThread.class}, atm, PID, thread);
            } finally { Binder.restoreCallingIdentity(identity); }
            set(process, "packageName", NAME); set(process, "processName", NAME);
            set(process, "packageCode", owner); set(process, "ready", true);
            synchronized (atm) {
                call(atm, "registerAttachedProcess", new Class<?>[] {process.getClass()}, process);
            }
            ExternalStorageAccess.syncAll(pm);
            drain();
        }
        void asApp(Call action) throws Exception {
            long identity = Binder.clearCallingIdentity();
            try { Binder.restoreCallingIdentity(((long) UID << 32) | PID); action.run(); }
            finally { Binder.restoreCallingIdentity(identity); }
        }
        void mode(int mode) { pm.appOps().setUidMode(OP, owner.uid(), mode); }
        String stored() throws Exception { return Files.readString(ExternalStorageAccess.policyFile(NAME).toPath()); }
        void drain() throws Exception {
            ((ExecutorService) get(pm, "mPackageEventExecutor")).submit(() -> { }).get(5, TimeUnit.SECONDS);
            drainOps();
        }
        void drainOps() throws Exception {
            ((ExecutorService) get(pm.appOps(), "mEvents")).submit(() -> { }).get(5, TimeUnit.SECONDS);
        }
        boolean removing() {
            try { return (Boolean) call(pm, "isRemovalPending", new Class<?>[] {String.class}, NAME); }
            catch (Exception failure) { throw new AssertionError(failure); }
        }
        @Override public void close() throws Exception { drain(); contexts.close(); }
    }
    private static boolean mediaPermission(Harness h, String permission) throws Exception {
        Method method = type("MediaStoreProvider").getDeclaredMethod("hasEffectivePermission",
                PackageManagerService.class, ApkPackage.class, String.class);
        method.setAccessible(true);
        return (Boolean) method.invoke(null, h.pm, h.owner, permission);
    }
    /** Isolate identity transport; use the real provider permission adapter and index access rules. */
    private static Object mediaAccess(Harness h) throws Exception {
        Class<?> caller = type("MediaStoreIndex$Caller");
        Object identity = Proxy.newProxyInstance(caller.getClassLoader(), new Class<?>[] {caller},
                (proxy, method, args) -> switch (method.getName()) {
                    case "self" -> false;
                    case "packageName" -> NAME;
                    case "uid" -> UID;
                    case "sharedPackages" -> h.pm.getPackagesForUid(UID);
                    case "targetSdk" -> h.owner.parsed.getTargetSdkVersion();
                    case "permission" -> mediaPermission(h, (String) args[0]);
                    case "opMode" -> h.pm.appOps().checkOperation(
                            AppOpsManager.strOpToOp((String) args[0]), UID, NAME);
                    case "legacyStorageOp" -> h.owner.parsed.getTargetSdkVersion() < 29;
                    case "uriGranted" -> false;
                    case "canSeePackage" -> true;
                    default -> throw new UnsupportedOperationException(method.getName());
                });
        return make(type("MediaStoreIndex$Access"), new Class<?>[] {caller}, identity);
    }
    private static boolean mediaCollection(Harness h, String collection, boolean write) throws Exception {
        int match = field(type("MediaStoreIndex"), collection).getInt(null);
        return (Boolean) call(mediaAccess(h), "collection", new Class<?>[] {int.class, boolean.class}, match, write);
    }
    /** Delays/fails only file I/O, then invokes the unmodified atomic writer. */
    private static final class WriterBoundary implements AutoCloseable {
        final Field field = field(ExternalStorageAccess.class, "sWriter");
        final Object original = field.get(null);
        final AtomicInteger active = new AtomicInteger(), maximum = new AtomicInteger();
        WriterBoundary(BeforeWrite before) throws Exception {
            Class<?> writer = type("ExternalStorageAccess$SnapshotWriter");
            Method write = writer.getDeclaredMethod("write", ApkPackage.class, ExternalStorageAccess.Policy.class, File.class);
            write.setAccessible(true);
            field.set(null, Proxy.newProxyInstance(writer.getClassLoader(), new Class<?>[] {writer}, (proxy, method, args) -> {
                int current = active.incrementAndGet(); maximum.accumulateAndGet(current, Math::max);
                try {
                    before.run((ApkPackage) args[0], (ExternalStorageAccess.Policy) args[1], (File) args[2]);
                    try { return write.invoke(original, args); }
                    catch (InvocationTargetException failure) { throw failure.getCause(); }
                } finally { active.decrementAndGet(); }
            }));
        }
        @Override public void close() throws Exception { field.set(null, original); }
    }
    private static void mkdirs(Harness h) throws Exception {
        Services.StorageService storage = new Services.StorageService();
        File shared = new File(System.getenv("ANDROID_DATA"), "media/0");
        for (String sub : List.of("data", "obb", "media")) {
            String suffix = "/Android/" + sub + "/" + NAME + "/mkdirs-contract";
            for (String alias : List.of("/sdcard", "/storage/emulated/0", "/storage/self/primary")) {
                h.asApp(() -> storage.mkdirs(NAME, alias + suffix));
                check(new File(shared, suffix.substring(1)).isDirectory(), "mkdirs alias did not create its real target");
            }
        }
        h.mode(AppOpsManager.MODE_ALLOWED);
        for (String path : List.of("/sdcard/Download/not-an-app-dir", "/sdcard/Android/data/" + PEER + "/foreign",
                "/sdcard/Android/obb/" + PEER, "/sdcard/Android/media/" + PEER)) {
            h.asApp(() -> rejects(SecurityException.class, () -> storage.mkdirs(NAME, path),
                    "MANAGE_EXTERNAL_STORAGE must not broaden the mkdirs Binder helper"));
        }
        h.asApp(() -> rejects(SecurityException.class,
                () -> storage.mkdirs(PEER, "/sdcard/Android/data/" + PEER), "claimed foreign package accepted"));
        File parent = new File(shared, "Android/data/" + NAME + "/ordinary-file");
        Files.writeString(parent.toPath(), "retained");
        h.asApp(() -> rejects(IllegalStateException.class,
                () -> storage.mkdirs(NAME, parent.getPath()), "regular file reported as a prepared directory"));
        h.asApp(() -> rejects(IllegalStateException.class,
                () -> storage.mkdirs(NAME, new File(parent, "child").getPath()), "failed mkdirs returned success"));
        check(Files.readString(parent.toPath()).equals("retained"), "failed mkdirs changed the existing file");
        storage.mkdirs("android", "/sdcard/Download/system-mkdirs-contract");
        check(new File(shared, "Download/system-mkdirs-contract").isDirectory(), "trusted mkdirs alias was not mapped");
        h.mode(AppOpsManager.MODE_IGNORED);
    }
    private static void runtimeMedia(Harness h) throws Exception {
        h.pm.permissions().grantRuntimePermission(NAME, IMAGES, "default:0", 0);
        h.pm.permissions().grantRuntimePermission(NAME, AUDIO, "default:0", 0);
        check(mediaPermission(h, IMAGES), "MediaStore did not accept the granted image permission");
        h.pm.appOps().setUidMode(AppOpsManager.OP_READ_MEDIA_IMAGES, UID, AppOpsManager.MODE_IGNORED);
        check(h.pm.permissions().checkUidPermission(UID, IMAGES, 0) == PackageManager.PERMISSION_GRANTED,
                "AppOps denial should preserve the permission bit");
        check(!mediaPermission(h, IMAGES) && mediaPermission(h, AUDIO),
                "MediaStore permission adapter ignored the runtime AppOps denial");
        check(ExternalStorageAccess.computePolicy(h.pm, h.owner).mediaMask() == ExternalStorageAccess.MEDIA_AUDIO,
                "AppOps denial failed to remove only image access");
        check(h.stored().contains("media_mask=4\n"), "runtime AppOps denial did not reach the native policy file");
        check(!ExternalStorageAccess.canAccessPath(h.pm, UID, NAME, "/sdcard/Pictures/image.jpg", false)
                        && ExternalStorageAccess.canAccessPath(h.pm, UID, NAME, "/sdcard/Music/audio.mp3", false),
                "effective media paths do not follow runtime AppOps consent");
        h.pm.appOps().setUidMode(AppOpsManager.OP_READ_MEDIA_IMAGES, UID, AppOpsManager.MODE_ALLOWED);
        check(ExternalStorageAccess.computePolicy(h.pm, h.owner).mediaMask() == 5,
                "restored image AppOp did not restore the granted category");
        h.pm.permissions().revokeRuntimePermission(NAME, IMAGES, "default:0", 0, "test cleanup");
        h.pm.permissions().revokeRuntimePermission(NAME, AUDIO, "default:0", 0, "test cleanup");
    }
    private static void legacyMedia(boolean host) throws Exception {
        try (Harness h = new Harness(host, 28)) {
            h.mode(AppOpsManager.MODE_IGNORED);
            // A pre-Q app needs the installer's ordinary restricted-permission
            // allowlist as well as the user's grant, including on Windows/ART.
            ExternalStorageAccessTest.admitPackages(h.pm, List.of(h.owner));
            h.pm.permissions().grantRuntimePermission(NAME, READ, "default:0", 0);
            check(mediaCollection(h, "FILES", false),
                    "MediaStore did not preserve granted legacy collection access");
            h.pm.appOps().setUidMode(AppOpsManager.OP_READ_EXTERNAL_STORAGE, UID, AppOpsManager.MODE_IGNORED);
            check(h.pm.permissions().checkUidPermission(UID, READ, 0) == PackageManager.PERMISSION_GRANTED,
                    "legacy AppOps denial should preserve the runtime grant bit");
            check(!mediaCollection(h, "FILES", false) && !mediaCollection(h, "IMAGES_MEDIA", false),
                    "MediaStore legacy/media fallback ignored READ_EXTERNAL_STORAGE AppOps denial");
            h.pm.permissions().grantRuntimePermission(NAME, WRITE, "default:0", 0);
            check(mediaCollection(h, "IMAGES_MEDIA", true),
                    "MediaStore did not preserve granted legacy writes");
            h.pm.appOps().setUidMode(AppOpsManager.OP_WRITE_EXTERNAL_STORAGE, UID, AppOpsManager.MODE_IGNORED);
            check(!mediaCollection(h, "IMAGES_MEDIA", true),
                    "MediaStore legacy write fallback ignored WRITE_EXTERNAL_STORAGE AppOps denial");
            h.pm.permissions().revokeRuntimePermission(NAME, READ, "default:0", 0, "test cleanup");
            h.pm.permissions().revokeRuntimePermission(NAME, WRITE, "default:0", 0, "test cleanup");

            // Fresh install after boot migration: an explicit installer opt-out
            // must remain restricted, irrespective of the fixture's earlier UIDs.
            ApkPackage restricted = pkg("fixture.storage.restricted28", 13081, 28);
            call(restricted, "setRestrictedPermissionAllowlist", new Class<?>[] {Collection.class}, List.of());
            call(h.pm, "add", new Class<?>[] {ApkPackage.class}, restricted);
            h.drain();
            int uid = restricted.uid();
            String name = restricted.packageName;
            int flags = h.pm.permissions().getPermissionFlags(name, WRITE, "default:0", 0);
            check((flags & PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION) != 0
                            && (flags & PackageManager.FLAGS_PERMISSION_RESTRICTION_ANY_EXEMPT) == 0,
                    "installer opt-out did not preserve the storage restriction");
            h.pm.permissions().grantRuntimePermission(name, WRITE, "default:0", 0);
            check(h.pm.permissions().checkUidPermission(uid, WRITE, 0) == PackageManager.PERMISSION_DENIED,
                    "restricted legacy package received a runtime storage grant");
            check(h.pm.appOps().checkRawMode(AppOpsManager.OP_WRITE_EXTERNAL_STORAGE, uid, name)
                            == AppOpsManager.MODE_IGNORED,
                    "restricted legacy package received the storage AppOp");
            check(ExternalStorageAccess.computePolicy(h.pm, restricted).mode() == ExternalStorageAccess.Mode.SCOPED,
                    "restricted legacy package escaped scoped storage");

            // The original mode fixture uses this same real admission helper.
            // Exemption alone is not consent; the explicit runtime grant is still required.
            ExternalStorageAccessTest.admitPackages(h.pm, List.of(restricted));
            flags = h.pm.permissions().getPermissionFlags(name, WRITE, "default:0", 0);
            check((flags & PackageManager.FLAG_PERMISSION_RESTRICTION_INSTALLER_EXEMPT) != 0
                            && (flags & PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION) == 0,
                    "ordinary installer admission did not reconcile the restricted permission");
            check(h.pm.permissions().checkUidPermission(uid, WRITE, 0) == PackageManager.PERMISSION_DENIED,
                    "installer allowlisting invented runtime consent");
            h.pm.permissions().grantRuntimePermission(name, WRITE, "default:0", 0);
            check(h.pm.permissions().checkUidPermission(uid, WRITE, 0) == PackageManager.PERMISSION_GRANTED,
                    "allowlisted legacy package did not receive the explicit runtime grant");
            check(h.pm.appOps().checkRawMode(AppOpsManager.OP_WRITE_EXTERNAL_STORAGE, uid, name)
                            == AppOpsManager.MODE_ALLOWED,
                    "allowlisted legacy runtime grant did not enable its AppOp");
            check(ExternalStorageAccess.computePolicy(h.pm, restricted).mode() == ExternalStorageAccess.Mode.LEGACY_RW,
                    "effective legacy write grant did not enable the storage policy");
            File policyFile = ExternalStorageAccess.policyFile(name);
            check(Files.readString(policyFile.toPath()).contains("mode=LEGACY_RW\n"),
                    "effective legacy grant was not published to the native policy file");
            h.pm.permissions().revokeRuntimePermission(name, WRITE, "default:0", 0, "test cleanup");
            check(ExternalStorageAccess.computePolicy(h.pm, restricted).mode() == ExternalStorageAccess.Mode.SCOPED
                            && Files.readString(policyFile.toPath()).contains("mode=SCOPED\n"),
                    "legacy storage access survived revocation");
        }
    }
    private static void grantThenRevoke(Harness h) throws Exception {
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicBoolean first = new AtomicBoolean(true);
        try (WriterBoundary disk = new WriterBoundary((pkg, policy, target) -> {
            if (pkg == h.owner && policy.mode() == ExternalStorageAccess.Mode.MANAGER && first.compareAndSet(true, false)) {
                entered.countDown(); waitFor(release);
            }
        })) {
            FutureTask<Void> grant = worker("storage-grant", () -> h.mode(AppOpsManager.MODE_ALLOWED));
            check(entered.await(5, TimeUnit.SECONDS), "grant did not reach the real final file publication");
            FutureTask<Void> revoke = worker("storage-revoke", () -> h.mode(AppOpsManager.MODE_IGNORED));
            try {
                await(() -> h.pm.appOps().checkRawMode(OP, UID, NAME) == AppOpsManager.MODE_IGNORED,
                        "concurrent revoke did not publish its desired policy");
                worker("storage-monitor-reader", () -> {
                    synchronized (h.pm) { synchronized (h.pm.permissions()) { } }
                }).get(3, TimeUnit.SECONDS);
            } finally { release.countDown(); }
            grant.get(5, TimeUnit.SECONDS); revoke.get(5, TimeUnit.SECONDS);
            check(disk.maximum.get() == 1, "storage snapshots were written concurrently");
            check(h.stored().contains("mode=SCOPED\n") && !ExternalStorageAccess.isExternalStorageManager(h.pm, h.owner),
                    "a returned revoke was overwritten by an older grant publication");
        } finally { release.countDown(); }
    }
    private static void queuedSnapshotAfterRevoke(Harness h) throws Exception {
        h.mode(AppOpsManager.MODE_ALLOWED);
        Object publication = field(ExternalStorageAccess.class, "PUBLICATION_LOCK").get(null);
        FutureTask<Void> pending = new FutureTask<>(() -> {
            ExternalStorageAccess.syncPackage(h.pm, h.owner); return null;
        });
        Thread older = new Thread(pending, "storage-queued-snapshot"); older.setDaemon(true);
        // Control only entry to the file-publication monitor. The old call computes
        // its real grant, then queues; the ordinary revoke completes first.
        synchronized (publication) {
            older.start();
            await(() -> {
                StackTraceElement[] stack = older.getStackTrace();
                return older.getState() == Thread.State.BLOCKED && stack.length != 0
                        && stack[0].getClassName().equals(ExternalStorageAccess.class.getName())
                        && stack[0].getMethodName().equals("syncPackage");
            }, "older snapshot did not queue at the file-publication boundary");
            h.mode(AppOpsManager.MODE_IGNORED);
            check(h.stored().contains("mode=SCOPED\n"), "completed revoke did not reach the policy file");
        }
        pending.get(5, TimeUnit.SECONDS);
        check(h.stored().contains("mode=SCOPED\n"), "queued older snapshot overwrote a completed revoke");
    }
    private static void failedGrantAndRetry(Harness h) throws Exception {
        h.drain();
        h.pm.permissions().grantRuntimePermission(NAME, IMAGES, "default:0", 0);
        AtomicInteger callbacks = new AtomicInteger(), allowedCallbacks = new AtomicInteger();
        IAppOpsCallback watcher = new IAppOpsCallback.Default() {
            final Binder token = new Binder();
            @Override public IBinder asBinder() { return token; }
            @Override public void opChanged(int op, int uid, String pkg, String device) {
                if (op == OP && uid == UID && NAME.equals(pkg)) {
                    callbacks.incrementAndGet();
                    if (h.pm.appOps().checkRawMode(OP, UID, NAME) == AppOpsManager.MODE_ALLOWED) {
                        allowedCallbacks.incrementAndGet();
                    }
                }
            }
        };
        h.pm.appOps().startWatchingMode(OP, NAME, watcher);
        CountDownLatch blocked = new CountDownLatch(1), release = new CountDownLatch(1);
        ((ExecutorService) get(h.pm, "mPackageEventExecutor")).execute(() -> {
            blocked.countDown();
            try { waitFor(release); } catch (IOException error) { throw new AssertionError(error); }
        });
        check(blocked.await(5, TimeUnit.SECONDS), "retirement callback boundary was not held");
        int oldExits = h.exits.get();
        try {
            try (WriterBoundary disk = new WriterBoundary((pkg, policy, target) -> {
                if (pkg == h.owner && policy.mode() == ExternalStorageAccess.Mode.MANAGER) {
                    throw new IOException("Isolated final storage-policy write failure");
                }
            })) {
                rejects(UncheckedIOException.class, () -> h.mode(AppOpsManager.MODE_ALLOWED),
                        "failed storage publication reported a successful grant");
                check(h.pm.appOps().checkRawMode(OP, UID, NAME) != AppOpsManager.MODE_ALLOWED
                                && !ExternalStorageAccess.isExternalStorageManager(h.pm, h.owner),
                        "failed publication remained an effective all-files grant");
                check(!ExternalStorageAccess.canAccessPath(h.pm, UID, NAME, "/sdcard/Download/shared", true),
                        "failed publication remained authorized through Java path policy");
                check(!mediaPermission(h, Manifest.permission.MANAGE_EXTERNAL_STORAGE)
                                && !mediaPermission(h, IMAGES),
                        "failed publication remained authorized through MediaStore permission mapping");
                File file = ExternalStorageAccess.policyFile(NAME);
                check(!file.exists() || Files.readString(file.toPath()).contains("mode=SCOPED\n"),
                        "failed final publication left an active manager snapshot");
                h.drainOps();
                check(callbacks.get() == 0, "failed grant notified a successful mode change");
            }
            h.mode(AppOpsManager.MODE_ALLOWED);
            check(h.pm.appOps().checkRawMode(OP, UID, NAME) == AppOpsManager.MODE_ALLOWED
                            && h.stored().contains("mode=MANAGER\n"), "retry did not recover the requested grant");
            check(mediaPermission(h, Manifest.permission.MANAGE_EXTERNAL_STORAGE) && mediaPermission(h, IMAGES),
                    "MediaStore permission mapping did not recover after successful publication");
            await(() -> callbacks.get() == 1 && allowedCallbacks.get() == 1,
                    "successful retry did not notify its effective allowed transition");
        } finally {
            release.countDown();
            h.pm.appOps().stopWatchingMode(watcher);
        }
        h.drain();
        check(h.exits.get() == oldExits, "old publication failure retired a successfully recovered generation");
        h.mode(AppOpsManager.MODE_IGNORED);
        h.pm.permissions().revokeRuntimePermission(NAME, IMAGES, "default:0", 0, "test cleanup");
    }
    private static void unrelatedPermission(Harness h) throws Exception {
        AtomicInteger storageWrites = new AtomicInteger();
        try (WriterBoundary disk = new WriterBoundary((pkg, policy, target) -> {
            storageWrites.incrementAndGet();
            throw new IOException("Isolated unavailable storage publisher");
        })) {
            h.pm.permissions().grantRuntimePermission(NAME, Manifest.permission.CAMERA, "default:0", 0);
            check(h.pm.permissions().checkUidPermission(UID, Manifest.permission.CAMERA, 0)
                            == PackageManager.PERMISSION_GRANTED, "storage I/O blocked an unrelated CAMERA grant");
            h.pm.appOps().setUidMode(AppOpsManager.OP_CAMERA, UID, AppOpsManager.MODE_IGNORED);
            check(h.pm.appOps().checkRawMode(AppOpsManager.OP_CAMERA, UID, NAME) == AppOpsManager.MODE_IGNORED,
                    "storage I/O blocked an unrelated runtime AppOps denial");
            check(storageWrites.get() == 0, "unrelated permission changes attempted storage publication");
        }
        h.pm.permissions().revokeRuntimePermission(NAME, Manifest.permission.CAMERA, "default:0", 0, "test cleanup");
    }
    private static void removalAndReplacement(Harness h) throws Exception {
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicBoolean first = new AtomicBoolean(true);
        try (WriterBoundary disk = new WriterBoundary((pkg, policy, target) -> {
            if (pkg == h.owner && policy.mode() == ExternalStorageAccess.Mode.MANAGER && first.compareAndSet(true, false)) {
                entered.countDown(); waitFor(release);
            }
        })) {
            FutureTask<Void> grant = worker("storage-old-generation", () -> h.mode(AppOpsManager.MODE_ALLOWED));
            check(entered.await(5, TimeUnit.SECONDS), "old generation did not reach file publication");
            FutureTask<Void> removal = worker("storage-remove", () -> {
                call(h.pm, "beginPackageRemoval", new Class<?>[] {ApkPackage.class}, h.owner);
                call(h.pm, "revokePackageCapabilities", new Class<?>[] {String.class}, NAME);
                h.pm.permissions().forgetPackage(NAME);
                call(h.pm, "remove", new Class<?>[] {String.class}, NAME);
                call(h.pm, "endPackageRemoval", new Class<?>[] {ApkPackage.class}, h.owner);
            });
            try {
                await(h::removing, "package removal did not enter its precommit retirement");
                check(!removal.isDone(), "removal completed before the blocked policy publication retired");
            }
            finally { release.countDown(); }
            grant.get(5, TimeUnit.SECONDS); removal.get(5, TimeUnit.SECONDS);
            check(!ExternalStorageAccess.policyFile(NAME).exists(), "old publication survived completed removal");
        } finally { release.countDown(); }
        ApkPackage replacement = pkg(NAME, UID);
        call(h.pm, "add", new Class<?>[] {ApkPackage.class}, replacement); h.drain();
        String current = h.stored();
        ExternalStorageAccess.syncPackage(h.pm, h.owner);
        check(h.stored().equals(current), "retired APK overwrote its replacement's storage policy");
    }
    private static void sharedUidFailure() throws Exception {
        ApkPackage first = pkg("fixture.storage.sharedfirst", 13001);
        ApkPackage second = pkg("fixture.storage.sharedsecond", 13001);
        for (ApkPackage member : List.of(first, second)) {
            ((com.android.internal.pm.parsing.pkg.PackageImpl) member.parsed)
                    .setSharedUserId("fixture.storage.shared");
        }
        PackageManagerService pm = (PackageManagerService) make(PackageManagerService.class,
                new Class<?>[] {Collection.class, boolean.class}, List.of(first, second), false);
        int uid = first.uid();
        check(uid == second.uid(), "shared-UID fixture members were not admitted to one principal");
        Object atm = make(type("ActivityTaskManagerService"), new Class<?>[] {PackageManagerService.class}, pm);
        Object am = make(type("ActivityManagerService"), new Class<?>[] {atm.getClass(), PackageManagerService.class,
                type("NotificationManagerService")}, atm, pm, null);
        try (FixtureServiceContext.Scope contexts = new FixtureServiceContext.Scope()) {
            field(Services.class, "sLast").set(null, contexts.runtime(pm, atm, am));
            pm.appOps().setUidMode(OP, uid, AppOpsManager.MODE_IGNORED);
            List<String> notifications = new java.util.concurrent.CopyOnWriteArrayList<>();
            IAppOpsCallback watcher = new IAppOpsCallback.Default() {
                final Binder token = new Binder();
                @Override public IBinder asBinder() { return token; }
                @Override public void opChanged(int op, int callerUid, String name, String device) {
                    if (op == OP && callerUid == uid) notifications.add(name);
                }
            };
            pm.appOps().startWatchingMode(OP, null, watcher);
            try {
                try (WriterBoundary disk = new WriterBoundary((pkg, policy, target) -> {
                    if (pkg == first && policy.mode() == ExternalStorageAccess.Mode.MANAGER) {
                        throw new IOException("Isolated first shared-UID member write failure");
                    }
                })) {
                    rejects(UncheckedIOException.class,
                            () -> pm.appOps().setUidMode(OP, uid, AppOpsManager.MODE_ALLOWED),
                            "partial shared-UID publication failure was hidden");
                    check(pm.appOps().checkRawMode(OP, uid, first.packageName) != AppOpsManager.MODE_ALLOWED,
                            "failed shared-UID member reported effective access");
                    File file = ExternalStorageAccess.policyFile(second.packageName);
                    boolean published = file.isFile() && Files.readString(file.toPath()).contains("mode=MANAGER\n");
                    boolean allowed = pm.appOps().checkRawMode(OP, uid, second.packageName) == AppOpsManager.MODE_ALLOWED;
                    check(published == allowed, "later shared-UID member reports a grant absent from its policy file");
                    check(published, "first member failure prevented the later member's independent publication");
                    await(() -> notifications.size() == 1, "successful shared-UID member was not notified after partial failure");
                    check(notifications.equals(List.of(second.packageName)),
                            "partial publication notified the failed member or omitted the successful member");
                }
                pm.appOps().setUidMode(OP, uid, AppOpsManager.MODE_ALLOWED);
                await(() -> notifications.size() == 2, "recovered shared-UID member was not notified");
                check(notifications.equals(List.of(second.packageName, first.packageName)),
                        "shared-UID retry did not notify only the newly effective member");
            } finally { pm.appOps().stopWatchingMode(watcher); }
            ((ExecutorService) get(pm, "mPackageEventExecutor")).submit(() -> { }).get(5, TimeUnit.SECONDS);
        }
    }
    public static int run(boolean host) throws Exception {
        checks = 0;
        try (Harness harness = new Harness(host)) {
            mkdirs(harness);
            grantThenRevoke(harness);
            queuedSnapshotAfterRevoke(harness);
            failedGrantAndRetry(harness);
            unrelatedPermission(harness);
            runtimeMedia(harness);
            removalAndReplacement(harness);
        }
        legacyMedia(host);
        sharedUidFailure();
        return checks;
    }
    public static void main(String[] args) throws Exception {
        System.out.println("ExternalStoragePolicyContractTest passed: " + run(false) + " checks");
    }
}
