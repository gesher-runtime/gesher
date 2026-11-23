/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PropertyInvalidatedCache;
import android.service.notification.StatusBarNotification;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.*;
import sun.misc.Unsafe;

/** Actual notification publication/lifecycle owner; constructed package metadata and codec boundary. */
public final class NotificationImagePublicationTest {
    private static int checks;
    private static final Unsafe UNSAFE;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (Exception e) { throw new ExceptionInInitializerError(e); }
    }
    private static void check(boolean value, String why) { if (!value) throw new AssertionError(why); checks++; }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static void set(Object value, String name, Object data) throws Exception { field(value.getClass(), name).set(value, data); }
    private static void await(CountDownLatch latch) {
        try { if (!latch.await(5, TimeUnit.SECONDS)) throw new AssertionError("publication fixture stalled"); }
        catch (InterruptedException e) { throw new AssertionError(e); }
    }
    private interface Work { void run() throws Exception; }
    private static final AtomicReference<Throwable> failure = new AtomicReference<>();
    private static Thread thread(String name, Work work) {
        Thread t = new Thread(() -> { try { work.run(); } catch (Throwable e) { failure.compareAndSet(null, e); } }, name);
        t.setDaemon(true); t.start(); return t;
    }
    private static void join(Thread t) throws Exception {
        t.join(5000); check(!t.isAlive(), "publication worker deadlocked: " + t.getName());
        if (failure.get() != null) throw new AssertionError("publication worker failed", failure.get());
    }
    private static final class Fixture {
        final String a = "fixture.notification.images.a", b = "fixture.notification.images.b";
        final Map<String, ApkPackage> packages = new LinkedHashMap<>();
        final PackageManagerService pm;
        final NotificationManagerService service;
        final AtomicInteger shown = new AtomicInteger();
        final AtomicReference<StatusBarNotification> lastShown = new AtomicReference<>();
        final Map<Integer, Runnable> encoders = new ConcurrentHashMap<>();

        @SuppressWarnings("unchecked") Fixture(boolean codec) throws Exception {
            pm = (PackageManagerService)UNSAFE.allocateInstance(PackageManagerService.class);
            set(pm, "mPackages", packages); set(pm, "mRemoving", new LinkedHashMap<>());
            packages.put(a, code(a)); packages.put(b, code(b));
            service = new NotificationManagerService(pm, null, (n, update) -> {
                shown.incrementAndGet(); lastShown.set(n); return 0;
            });
            Map<String, NotificationChannel> channels = (Map<String, NotificationChannel>)field(NotificationManagerService.class, "mChannels").get(service);
            Set<String> loaded = (Set<String>)field(NotificationManagerService.class, "mLoadedChannelPackages").get(service);
            for (String pkg : packages.keySet()) {
                channels.put(pkg + "/images", new NotificationChannel("images", "Images", NotificationManager.IMPORTANCE_DEFAULT));
                loaded.add(pkg);
            }
            if (codec) {
                BiFunction<StatusBarNotification, Object, NotificationImageStore.Prepared> prepare = (n, owner) -> {
                    try {
                        File target = image(n);
                        return NotificationImageStore.prepare(target, owner, new NotificationImageStore.Image() {
                            public byte[] signature() { return new byte[]{(byte)n.getId()}; }
                            public boolean unchanged() { return true; }
                            public boolean encode(OutputStream out) throws IOException {
                                Runnable blocked = encoders.get(n.getId()); if (blocked != null) blocked.run();
                                out.write((n.getPackageName() + ":" + n.getId()).getBytes(java.nio.charset.StandardCharsets.UTF_8));
                                return true;
                            }
                        });
                    } catch (IOException e) { return null; }
                    catch (InterruptedException e) { Thread.currentThread().interrupt(); return null; }
                    catch (ReflectiveOperationException e) { throw new AssertionError(e); }
                };
                set(service, "mTestImagePreparation", prepare);
            }
        }
        ApkPackage code(String name) throws Exception {
            ApkPackage pkg = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
            PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/isolated-fixture/" + name + ".apk");
            parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.hideAsFinal();
            set(pkg, "parsed", parsed); set(pkg, "packageName", name); set(pkg, "mUid", 10000);
            return pkg;
        }
        Notification notification() throws Exception {
            Notification n = new Notification(); field(Notification.class, "mChannelId").set(n, "images"); return n;
        }
        void post(String pkg, int id) throws Exception { service.postFor(packages.get(pkg), pkg, null, id, notification()); }
        @SuppressWarnings("unchecked") int active() throws Exception {
            synchronized (service) { return ((Map<?, ?>)field(NotificationManagerService.class, "mPosted").get(service)).size(); }
        }
        @SuppressWarnings("unchecked") int preparing() throws Exception {
            Object lock = field(NotificationManagerService.class, "mPublicationLock").get(service);
            synchronized (lock) { return ((Set<?>)field(NotificationManagerService.class, "mPreparingImages").get(service)).size(); }
        }
        File image(StatusBarNotification n) throws ReflectiveOperationException {
            Method path = NotificationIcons.class.getDeclaredMethod("largeFile", String.class, String.class); path.setAccessible(true);
            return (File)path.invoke(null, n.getPackageName(), n.getKey());
        }
    }

    private static void cancellationProbe() throws Exception {
        Fixture f = new Fixture(false); CountDownLatch entered = new CountDownLatch(1), resume = new CountDownLatch(1);
        set(f.service, "mBeforePostPublication", (Runnable)() -> { entered.countDown(); await(resume); });
        Thread t = thread("pre-publication", () -> f.post(f.a, 1)); await(entered);
        f.service.cancel(f.a, null, 1); resume.countDown(); join(t);
        check(f.shown.get() == 0 && f.active() == 0, "cancellation was undone by an earlier preparing post");
    }

    private static void codecProgress() throws Exception {
        Fixture f = new Fixture(true); CountDownLatch entered = new CountDownLatch(1), resume = new CountDownLatch(1);
        f.encoders.put(2, () -> { entered.countDown(); await(resume); });
        Thread slow = thread("blocked-image-A", () -> f.post(f.a, 2)); await(entered);
        Thread fast = thread("unrelated-image-B", () -> f.post(f.b, 3)); join(fast);
        check(f.shown.get() == 1 && slow.isAlive(), "one package's encoder blocked unrelated publication");
        Thread cancel = thread("cancel-blocked-image", () -> f.service.cancel(f.a, null, 2)); join(cancel);
        resume.countDown(); join(slow);
        check(f.shown.get() == 1 && f.active() == 1 && f.preparing() == 0, "stale encode resurrected notification");
        f.service.cancel(f.b, null, 3);
        check(f.active() == 0, "image cancellation left active state");
    }

    private static void generationAndFailure() throws Exception {
        Fixture f = new Fixture(true); CountDownLatch entered = new CountDownLatch(1), resume = new CountDownLatch(1);
        f.encoders.put(4, () -> { entered.countDown(); await(resume); });
        Thread slow = thread("old-package-image", () -> f.post(f.a, 4)); await(entered);
        f.service.packageRemoved(f.a); f.packages.put(f.a, f.code(f.a)); resume.countDown(); join(slow);
        check(f.shown.get() == 0 && f.active() == 0 && f.preparing() == 0, "reinstall accepted old package image");
        Fixture broken = new Fixture(true);
        broken.encoders.put(5, () -> { throw new IllegalStateException("injected codec failure"); });
        try { broken.post(broken.a, 5); throw new AssertionError("injected boundary failure did not propagate"); }
        catch (IllegalStateException expected) { checks++; }
        check(broken.active() == 0 && broken.preparing() == 0, "failed preparation leaked publication owner");
        broken.encoders.clear(); broken.post(broken.b, 6);
        check(broken.shown.get() == 1, "failed preparation leaked lane/encoder credit");
    }

    private static void cancelAndReplace() throws Exception {
        Fixture f = new Fixture(true); CountDownLatch entered = new CountDownLatch(1), resume = new CountDownLatch(1);
        f.encoders.put(9, () -> { entered.countDown(); await(resume); });
        Thread first = thread("replace-old-encoder", () -> f.post(f.a, 9)); await(entered);
        Thread second = thread("replace-old-waiter", () -> f.post(f.a, 9));
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (f.preparing() != 2 && System.nanoTime() < deadline) Thread.yield();
        check(f.preparing() == 2, "same-key replacement did not acquire its cancellation token");
        f.service.cancel(f.a, null, 9);
        f.encoders.clear();
        Notification replacement = f.notification(); replacement.tickerText = "after cancellation";
        Thread latest = thread("replacement-after-cancel", () -> f.service.postFor(f.packages.get(f.a), f.a, null, 9, replacement));
        resume.countDown(); join(first); join(second); join(latest);
        check(f.shown.get() == 1 && f.active() == 1 && f.preparing() == 0
                && "after cancellation".contentEquals(f.lastShown.get().getNotification().tickerText),
                "cancelled preparers resurrected state or deleted a later replacement");
        check(f.image(f.lastShown.get()).isFile(), "later replacement lost its published PNG");
        f.service.cancel(f.a, null, 9);
        check(!f.image(f.lastShown.get()).exists(), "replacement image survived cancellation");
    }

    private static void failedNativeRemoval() throws Exception {
        Fixture f = new Fixture(true); f.post(f.a, 10);
        File current = f.image(f.lastShown.get());
        File legacy = new File(NotificationIcons.dir(), current.getName());
        Files.writeString(legacy.toPath(), "retained legacy toast image");
        f.post(f.a, 10);
        check(legacy.isFile(), "update removed a legacy image without proving native retirement");
        set(f.service, "mTestToastRemoval", (ToIntBiFunction<String, String>)(tag, key) -> 1);
        f.service.cancel(f.a, null, 10);
        check(current.isFile() && legacy.isFile(), "failed native removal freed retained images");
        f.service.cancel(f.a, null, 10);
        check(current.isFile() && legacy.isFile(), "repeated cancellation inferred native success from missing metadata");
        // The test toast boundary explicitly reports successful package-wide
        // history removal here; only that proof permits the new-layout cleanup.
        f.service.packageRemoved(f.a);
        check(!current.exists(), "successful history clear retained a current-layout PNG");
        Files.deleteIfExists(legacy.toPath());
    }

    private static void reentrantLifecycle() throws Exception {
        Fixture f = new Fixture(true); CountDownLatch entered = new CountDownLatch(1), resume = new CountDownLatch(1);
        f.encoders.put(7, () -> { entered.countDown(); await(resume); });
        Thread first = thread("ordinary-post", () -> f.post(f.a, 7)); await(entered);
        CountDownLatch associated = new CountDownLatch(1);
        Thread foreground = thread("foreground-lifecycle", () -> f.service.withChannelLifecycle(() -> {
            try { f.post(f.a, 7); associated.countDown(); }
            catch (Exception e) { throw new RuntimeException(e); }
        }));
        await(associated);
        Thread cancel = thread("lifecycle-cancel", () -> f.service.cancel(f.a, null, 7)); join(cancel);
        resume.countDown(); join(first); join(foreground);
        check(f.shown.get() == 0 && f.active() == 0 && f.preparing() == 0,
                "deferred lifecycle image resurrected state or deadlocked cancellation");
        f.encoders.clear();
        f.service.withChannelLifecycle(() -> { try { f.post(f.b, 8); } catch (Exception e) { throw new RuntimeException(e); } });
        check(f.shown.get() == 1 && f.active() == 1, "valid deferred lifecycle post was lost");
        f.encoders.put(11, () -> { throw new IllegalStateException("injected deferred codec failure"); });
        try {
            f.service.withChannelLifecycle(() -> {
                try { f.post(f.a, 11); f.post(f.b, 12); }
                catch (Exception e) { throw new RuntimeException(e); }
            });
            throw new AssertionError("deferred failure was hidden");
        } catch (IllegalStateException expected) { checks++; }
        check(f.shown.get() == 2 && f.preparing() == 0, "a failed deferred post stranded later owners");
    }

    private static void pendingUijDetach(boolean existing, boolean detach) throws Exception {
        Fixture f = new Fixture(true);
        if (existing) {
            Notification visible = f.notification(); visible.flags |= Notification.FLAG_USER_INITIATED_JOB;
            f.service.postFor(f.packages.get(f.a), f.a, null, 73, visible);
        }
        CountDownLatch entered = new CountDownLatch(1), resume = new CountDownLatch(1);
        set(f.service, "mBeforePostPublication", (Runnable)() -> { entered.countDown(); await(resume); });
        Notification notification = f.notification(); notification.flags |= Notification.FLAG_USER_INITIATED_JOB;
        Thread post = thread("pending-uij", () -> f.service.withChannelLifecycle(() ->
                f.service.postFor(f.packages.get(f.a), f.a, null, 73, notification)));
        try {
            await(entered);
            if (detach) {
                Thread finish = thread("pending-uij-detach", () -> f.service.withChannelLifecycle(() ->
                        f.service.removeUserInitiatedJobFlag(f.a, 73)));
                join(finish);
            } else {
                // A final association of an unrelated key cannot strip this
                // still-active UIJ while it is waiting to publish.
                f.service.withChannelLifecycle(() -> f.service.removeUserInitiatedJobFlag(f.a, 74));
            }
        } finally { resume.countDown(); }
        join(post);
        check(f.active() == 1 && f.preparing() == 0, "DETACH lost the retained notification or preparation owner");
        check(((f.lastShown.get().getNotification().flags & Notification.FLAG_USER_INITIATED_JOB) != 0) == !detach,
                "Deferred UIJ publication ignored its current association state");
        check(f.image(f.lastShown.get()).isFile(), "UIJ flag update lost the prepared image");
        f.service.cancel(f.a, null, 73);
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 0) {
            System.loadLibrary("android_runtime");
            File data = Files.createTempDirectory(new File(System.getenv("ANDROID_DATA")).toPath(),
                    "notification-image-publication-").toFile();
            android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
            android.system.Os.setenv("GESHER_MICROG_SPOOF", "0", true);
            android.system.Os.unsetenv("GESHER_BINDER_UID");
        } else System.load(args[0]);
        PropertyInvalidatedCache.setTestMode(true);
        check(!GmsSignatureSpoofing.isEnabled(), "host fixture must use declared legacy permission boundary");
        cancellationProbe();
        if (args.length <= 1) {
            codecProgress(); generationAndFailure(); cancelAndReplace(); failedNativeRemoval(); reentrantLifecycle();
            pendingUijDetach(false, true); pendingUijDetach(true, true); pendingUijDetach(false, false);
        }
        System.out.println("NotificationImagePublicationTest passed: " + checks + " checks");
        System.out.println("Production publication/cache/file owner; constructed packages, explicit codec and toast boundaries.");
    }
}
