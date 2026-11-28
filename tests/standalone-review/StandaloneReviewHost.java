/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.app.IApplicationThread;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.Signature;
import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.FutureTask;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;
import sun.misc.Unsafe;

/** Exact admission/controller/policy code; APK metadata and native platform boundaries are fixtures. */
public final class StandaloneReviewHost {
    private static int checks;
    private static final Unsafe UNSAFE;
    static {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true);
            UNSAFE = (Unsafe)field.get(null);
        } catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws ReflectiveOperationException {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static void set(Object target, String name, Object value) throws ReflectiveOperationException {
        field(target.getClass(), name).set(target, value);
    }
    private static ApkPackage apk(String name, int target) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(target);
        parsed.addUsesPermission(new ParsedUsesPermissionImpl(Manifest.permission.CAMERA, 0, Set.of()));
        parsed.hideAsFinal();
        ApkPackage owner = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(owner, "packageName", name); set(owner, "parsed", parsed); set(owner, "mUid", 10000);
        set(owner, "apkPath", "/fixture/" + name + "/base.apk");
        set(owner, "mDataDir", "/fixture/" + name + "/data");
        set(owner, "sharedLibraries", SharedLibraryGraph.EMPTY);
        set(owner, "mSigningIdentityChecked", true);
        set(owner, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{7})});
        return owner;
    }
    private static boolean awaitReview(InProcessApplication admission) throws Exception {
        // Reflection also lets --source-ref test the pre-fix class without copying
        // or replacing its binding implementation just to make a control compile.
        try {
            return (Boolean)InProcessApplication.class.getMethod("awaitPermissionReview").invoke(admission);
        } catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception exception) throw exception;
            if (failure.getCause() instanceof Error error) throw error;
            throw failure;
        }
    }
    private static IApplicationThread applicationThread() {
        return new IApplicationThread.Default() {
            final Binder token = new Binder();
            @Override public IBinder asBinder() { return token; }
        };
    }

    private static final class Harness implements AutoCloseable {
        final ApkPackage owner;
        final PackageManagerService pm;
        final ActivityTaskManagerService atm;
        final ActivityManagerService am;
        final InProcessApplication admission;
        final BlockingQueue<Runnable> work = new LinkedBlockingQueue<>();
        final AtomicInteger prompts = new AtomicInteger();
        Thread startupThread;
        int choice = PermissionRequests.ALLOWED;
        @SuppressWarnings("unchecked") Harness(String name, int target) throws Exception {
            owner = apk("fixture.standalone." + name, target);
            pm = new PackageManagerService(List.of(), false);
            // Package publication stays real; host metadata tests do not send
            // package broadcasts into an unbootstrapped global ServiceManager.
            set(pm, "mPackageEventSender", (Consumer<Intent>)event -> { });
            Object catalog = field(PermissionPolicy.class, "mCatalog").get(pm.permissions());
            Map<String, PermissionInfo> definitions =
                    (Map<String, PermissionInfo>)field(catalog.getClass(), "mFramework").get(catalog);
            // The host has no framework-res.apk; this is CAMERA's pinned Android 16
            // dangerous declaration, not a replacement permission decision.
            PermissionInfo camera = new PermissionInfo();
            camera.name = Manifest.permission.CAMERA; camera.packageName = "android";
            camera.protectionLevel = PermissionInfo.PROTECTION_DANGEROUS;
            camera.group = "android.permission-group.UNDEFINED";
            definitions.put(camera.name, camera); set(catalog, "mLoaded", true);
            pm.add(owner);
            atm = new ActivityTaskManagerService(pm);
            am = new ActivityManagerService(atm, pm, null);
            PermissionRequests requests = new PermissionRequests(pm, work::add, new PermissionRequests.Decision() {
                @Override public int decide(String owner, String permissions) { throw new AssertionError("Unexpected runtime dialog"); }
                @Override public int decideBackground(String owner, String permissions) { throw new AssertionError("Unexpected background dialog"); }
                @Override public int decideReview(String name, String[] labels, boolean[] enabled, boolean[] changeable) {
                    check(name.equals(Harness.this.owner.packageName), "Review changed its admitted package identity");
                    check(!Thread.holdsLock(admission) && !Thread.holdsLock(pm) && !Thread.holdsLock(atm),
                            "Human consent ran under an admission or service monitor");
                    prompts.incrementAndGet(); return choice;
                }
            });
            InProcessApplication prepared;
            try {
                var constructor = InProcessApplication.class.getDeclaredConstructor(
                        ApkPackage.class, PackageManagerService.class, PermissionRequests.class);
                constructor.setAccessible(true);
                prepared = constructor.newInstance(owner, pm, requests);
            } catch (NoSuchMethodException oldSource) {
                prepared = new InProcessApplication(owner, pm);
            }
            admission = prepared;
            // Global ServiceManager bootstrap and Windows resources are outside
            // this host test. The actual installed services and admission remain
            // production objects; no bind/review/permission method is replaced.
            set(admission, "mServices", new Services.Created(pm, atm, am,
                    new ClipboardService(atm, pm), new UsbService(am.callers(), pm, am), Map.of()));
        }
        FutureTask<Boolean> start() {
            FutureTask<Boolean> result = new FutureTask<>(() -> {
                long previous = Binder.clearCallingIdentity();
                Binder.restoreCallingIdentity((10000L << 32) | (Process.myPid() & 0xffffffffL));
                try { return awaitReview(admission); }
                finally { Binder.restoreCallingIdentity(previous); }
            });
            startupThread = new Thread(result, "standalone-startup-fixture");
            startupThread.setDaemon(true); startupThread.start();
            return result;
        }
        Runnable pending() throws Exception {
            Runnable job = work.poll(5, TimeUnit.SECONDS);
            check(job != null, "Standalone startup did not dispatch its permission review");
            return job;
        }
        void requireUnbound() throws Exception {
            check(field(InProcessApplication.class, "mProcess").get(admission) == null,
                    "APK process was bound before review admission");
        }
        void requireBindRejected() throws Exception {
            boolean rejected = false;
            try { admission.bind(applicationThread()); }
            catch (IllegalStateException | SecurityException expected) { rejected = true; }
            check(rejected, "Standalone bind accepted an unreviewed/retired APK before Application.onCreate");
            requireUnbound();
        }
        @Override public void close() { admission.close(); }
    }

    private static void choices() throws Exception {
        for (int decision : new int[]{PermissionRequests.DISMISSED, PermissionRequests.UNAVAILABLE,
                PermissionRequests.ALLOWED}) {
            try (Harness h = new Harness("choice" + (decision + 2), 22)) {
                check(h.pm.permissions().isReviewRequired(h.owner), "Legacy fixture did not require review");
                h.requireBindRejected();
                h.choice = decision;
                FutureTask<Boolean> startup = h.start();
                Runnable review = h.pending();
                check(!startup.isDone(), "Standalone startup continued while consent was pending");
                h.requireUnbound();
                // A different thread must acquire every relevant monitor while
                // startup waits, rather than relying only on reentrant lock tests.
                FutureTask<Boolean> available = new FutureTask<>(() -> {
                    synchronized (h.admission) { synchronized (h.pm) { synchronized (h.atm) { return true; } } }
                });
                Thread probe = new Thread(available, "standalone-lock-probe"); probe.setDaemon(true); probe.start();
                check(available.get(5, TimeUnit.SECONDS), "Startup held a monitor during human review");
                review.run();
                check(startup.get(5, TimeUnit.SECONDS) == (decision == PermissionRequests.ALLOWED),
                        "Standalone startup ignored the explicit review decision");
                check(h.prompts.get() == 1, "Standalone startup duplicated or skipped its required prompt");
                if (decision != PermissionRequests.ALLOWED) {
                    h.requireBindRejected();
                } else {
                    h.admission.bind(applicationThread());
                    check(field(InProcessApplication.class, "mProcess").get(h.admission) != null,
                            "Explicitly reviewed startup could not bind its actual admission");
                }
            }
        }
    }

    private static void retired() throws Exception {
        for (String retirement : List.of("close", "replacement", "removal", "disabled")) {
            try (Harness h = new Harness(retirement, 22)) {
                FutureTask<Boolean> startup = h.start(); Runnable review = h.pending();
                switch (retirement) {
                    case "close" -> h.admission.close();
                    case "replacement" -> h.pm.add(apk(h.owner.packageName, 22));
                    case "removal" -> h.pm.remove(h.owner.packageName);
                    case "disabled" -> h.pm.setApplicationEnabledSetting(h.owner.packageName,
                            PackageManager.COMPONENT_ENABLED_STATE_DISABLED, 0, 0, "android");
                }
                if (retirement.equals("close")) {
                    check(!startup.get(5, TimeUnit.SECONDS), "Closed admission kept waiting for a human response");
                }
                review.run();
                check(!startup.get(5, TimeUnit.SECONDS), "Obsolete standalone admission resumed after review");
                h.requireBindRejected();
            }
        }
    }

    private static void modernAndChangedPolicy() throws Exception {
        try (Harness h = new Harness("modern", 36)) {
            FutureTask<Boolean> startup = h.start();
            check(startup.get(5, TimeUnit.SECONDS), "Modern standalone startup was blocked by legacy review");
            check(h.prompts.get() == 0 && h.work.isEmpty(),
                    "Modern standalone startup depended on the human-review worker");
            h.admission.bind(applicationThread());
            check(field(InProcessApplication.class, "mProcess").get(h.admission) != null,
                    "Modern standalone startup did not bind");
        }
        try (Harness h = new Harness("policychange", 22)) {
            FutureTask<Boolean> startup = h.start(); h.pending().run();
            check(startup.get(5, TimeUnit.SECONDS), "Initial legacy review did not complete");
            h.pm.permissions().updatePermissionFlags(h.owner.packageName, Manifest.permission.CAMERA,
                    PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED, PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED,
                    true, "default:0", 0);
            h.requireBindRejected();
        }
    }

    private static void interrupted() throws Exception {
        try (Harness h = new Harness("interrupted", 22)) {
            FutureTask<Boolean> startup = h.start(); Runnable review = h.pending();
            h.startupThread.interrupt();
            boolean interrupted = false;
            try { startup.get(5, TimeUnit.SECONDS); }
            catch (ExecutionException failure) { interrupted = failure.getCause() instanceof InterruptedException; }
            check(interrupted, "Interrupted standalone startup kept waiting or reported approval");
            review.run();
            check(!h.pm.permissions().isReviewRequired(h.owner), "Late explicit review approval did not exercise the race");
            h.requireBindRejected();
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        field(locales, "sIs24Hour").set(null, Boolean.TRUE);
        field(locales, "sLocales").set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        long previous = Binder.clearCallingIdentity();
        Binder.restoreCallingIdentity((10000L << 32) | (Process.myPid() & 0xffffffffL));
        try { choices(); retired(); modernAndChangedPolicy(); interrupted(); }
        finally { Binder.restoreCallingIdentity(previous); }
        System.out.println("Standalone permission review: " + checks + " checks passed");
        System.out.println("Actual admission, controller, policy, and process binding; declared host metadata/native identity boundaries. Windows UI, ART, Main bootstrap and APK execution remain separate runtime gates.");
    }
}
