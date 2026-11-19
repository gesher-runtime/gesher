/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.AppOpsManager;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.Signature;
import android.content.pm.PermissionInfo;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.StandIn;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import sun.misc.Unsafe;

/** Android 16 r4 registration contracts through the actual ActivityManager entry point. */
public final class ReceiverExportContractTest {
    private static final String ACTION = "fixture.receiver.exports.EVENT";
    private static int checks;
    private static final Unsafe UNSAFE;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Checked { void run() throws Exception; }
    private static void check(boolean condition, String why) {
        if (!condition) throw new AssertionError(why);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Object get(Object object, String name) throws Exception { return field(object.getClass(), name).get(object); }
    private static void set(Object object, String name, Object value) throws Exception { field(object.getClass(), name).set(object, value); }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Object make(Class<?> type, Class<?>[] signature, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(signature); c.setAccessible(true); return c.newInstance(args);
    }
    private static void rejects(Class<? extends Throwable> expected, Checked action) throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            if (failure instanceof InvocationTargetException wrapped) failure = wrapped.getCause();
            if (expected.isInstance(failure)) { checks++; return; }
            throw new AssertionError("Expected " + expected.getSimpleName() + ", got " + failure, failure);
        }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }
    private static void as(int uid, int pid, Checked action) throws Exception {
        long previous = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)uid << 32) | (pid & 0xffffffffL)); action.run(); }
        finally { Binder.restoreCallingIdentity(previous); }
    }
    private static ApkPackage pkg(String name, int uid, int sdk) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(sdk);
        if (name.endsWith(".sharedold")) {
            parsed.addUsesPermission(new ParsedUsesPermissionImpl("android.permission.INTERNET", 0, Set.of()));
        }
        parsed.hideAsFinal();
        ApkPackage apk = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(apk, "packageName", name); set(apk, "parsed", parsed); set(apk, "mUid", uid);
        set(apk, "apkPath", "/fixture/" + name + "/base.apk"); set(apk, "mDataDir", "/fixture/" + name + "/data");
        set(apk, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        set(apk, "mSigningIdentityChecked", true);
        set(apk, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{8})});
        return apk;
    }
    private static final class Receiver extends IIntentReceiver.Default {
        final Binder token = new Binder();
        @Override public IBinder asBinder() { return token; }
    }
    private static final class ThreadStub extends IApplicationThread.Default {
        final Binder token = new Binder();
        int deliveries;
        Runnable beforeBinder;
        @Override public IBinder asBinder() {
            Runnable action = beforeBinder; beforeBinder = null;
            if (action != null) action.run();
            return token;
        }
        @Override public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent,
                int resultCode, String data, Bundle extras, boolean ordered, boolean sticky,
                boolean assumed, int userId, int processState, int sendingUid, String sendingPackage) {
            deliveries++;
        }
    }
    private static IBinder transport(IBinder target) {
        return new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, data, reply, flags);
            }
        };
    }
    private static final class App {
        final ApkPackage code;
        final int pid;
        final ThreadStub thread = new ThreadStub();
        Object process;
        App(String suffix, int uid, int sdk, int pid) throws Exception {
            code = pkg("fixture.receiver." + suffix, uid, sdk); this.pid = pid;
        }
        void call(Checked action) throws Exception { as(code.uid(), pid, action); }
    }
    private static final class Harness implements AutoCloseable {
        final App old = new App("sdk33", 12800, 33, 888001);
        final App modern = new App("sdk34", 12801, 34, 888002);
        final App latest = new App("sdk36", 12802, 36, 888003);
        final App sharedNew = new App("sharednew", 12803, 36, 888004);
        final App sharedOld = new App("sharedold", 12803, 33, 888005);
        final List<App> apps = List.of(old, modern, latest, sharedNew, sharedOld);
        final PackageManagerService pm;
        final Object atm, controller, catalog;
        final IActivityManager am;
        @SuppressWarnings("unchecked") Harness(boolean binder) throws Exception {
            List<ApkPackage> packages = new ArrayList<>();
            for (App app : apps) packages.add(app.code);
            pm = (PackageManagerService)make(PackageManagerService.class,
                    new Class<?>[]{Collection.class, boolean.class}, packages, false);
            atm = make(type("ActivityTaskManagerService"), new Class<?>[]{PackageManagerService.class}, pm);
            IActivityManager service = (IActivityManager)make(type("ActivityManagerService"),
                    new Class<?>[]{atm.getClass(), PackageManagerService.class, type("NotificationManagerService")}, atm, pm, null);
            controller = get(service, "mBroadcasts");
            catalog = get(pm.permissions(), "mCatalog");
            if (!binder) {
                // Host lacks framework-res. These definitions are in the pinned Android manifest.
                ((Set<String>)get(catalog, "mProtectedBroadcasts")).add(Intent.ACTION_BOOT_COMPLETED);
                ((Set<String>)get(catalog, "mProtectedBroadcasts")).add(Intent.ACTION_BATTERY_CHANGED);
                PermissionInfo internet = new PermissionInfo();
                internet.name = "android.permission.INTERNET"; internet.packageName = "android";
                internet.protectionLevel = PermissionInfo.PROTECTION_NORMAL;
                ((Map<String, PermissionInfo>)get(catalog, "mFramework")).put(internet.name, internet);
                set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            }
            for (App app : apps) app.call(() -> {
                app.process = make(type("ActivityTaskManagerService$ProcessRecord"),
                        new Class<?>[]{atm.getClass(), int.class, IApplicationThread.class}, atm, app.pid, app.thread);
                set(app.process, "packageName", app.code.packageName); set(app.process, "processName", app.code.packageName);
                set(app.process, "packageCode", app.code); set(app.process, "ready", true);
                ((List<Object>)get(atm, "mProcesses")).add(app.process);
            });
            am = binder ? IActivityManager.Stub.asInterface(transport(StandIn.of(IActivityManager.class, service).asBinder())) : service;
        }
        int registrations() throws Exception { return ((Map<?,?>)get(controller, "mRegistered")).size(); }
        void register(App app, IIntentReceiver receiver, IntentFilter filter, int flags) throws Exception {
            register(app, receiver, filter, flags, null);
        }
        void register(App app, IIntentReceiver receiver, IntentFilter filter, int flags,
                String permission) throws Exception {
            app.call(() -> am.registerReceiverWithFeature(app.thread, app.code.packageName, null,
                    null, receiver, filter, permission, 0, flags));
        }
        void allowed(App app, IntentFilter filter, int flags) throws Exception {
            Receiver receiver = new Receiver(); int before = registrations();
            register(app, receiver, filter, flags);
            check(registrations() == before + 1, "Accepted registration did not enter the real controller");
            app.call(() -> am.unregisterReceiver(receiver));
            check(registrations() == before, "Unregister leaked a receiver");
        }
        void denied(App app, IntentFilter filter, int flags, Class<? extends Throwable> error) throws Exception {
            int before = registrations();
            rejects(error, () -> register(app, new Receiver(), filter, flags));
            check(registrations() == before, "Rejected registration mutated receiver state");
        }
        void send(App app, String action, String target) throws Exception {
            app.call(() -> am.broadcastIntentWithFeature(app.thread, null, new Intent(action).setPackage(target),
                    null, null, 0, null, null, null, null, null, AppOpsManager.OP_NONE, null, false, false, 0));
        }
        @Override public void close() throws Exception {
            ((ScheduledThreadPoolExecutor)get(controller, "mTimeouts")).shutdownNow();
        }
    }
    public static int run(boolean binder) throws Exception {
        checks = 0;
        try (Harness h = new Harness(binder)) {
            IntentFilter custom = new IntentFilter(ACTION);
            for (App app : List.of(h.modern, h.latest)) {
                h.denied(app, custom, 0, SecurityException.class);
                h.denied(app, new IntentFilter(), 0, SecurityException.class);
                h.denied(app, custom, Context.RECEIVER_VISIBLE_TO_INSTANT_APPS, SecurityException.class);
                h.allowed(app, custom, Context.RECEIVER_EXPORTED);
                h.allowed(app, custom, Context.RECEIVER_NOT_EXPORTED);
                h.allowed(app, custom, Context.RECEIVER_EXPORTED | Context.RECEIVER_VISIBLE_TO_INSTANT_APPS);
                h.denied(app, custom, Context.RECEIVER_EXPORTED | Context.RECEIVER_NOT_EXPORTED, IllegalArgumentException.class);
                h.denied(app, custom, Context.RECEIVER_NOT_EXPORTED | Context.RECEIVER_VISIBLE_TO_INSTANT_APPS, IllegalArgumentException.class);
                IntentFilter protectedOnly = new IntentFilter(Intent.ACTION_BOOT_COMPLETED);
                h.allowed(app, protectedOnly, 0);
                h.denied(app, protectedOnly, Context.RECEIVER_NOT_EXPORTED | Context.RECEIVER_VISIBLE_TO_INSTANT_APPS, IllegalArgumentException.class);
                protectedOnly.addAction(ACTION);
                h.denied(app, protectedOnly, 0, SecurityException.class);
                int before = h.registrations();
                h.register(app, null, custom, 0);
                check(h.registrations() == before, "Sticky query created a registered receiver");
                rejects(IllegalArgumentException.class, () -> h.register(app, null, custom,
                        Context.RECEIVER_EXPORTED | Context.RECEIVER_NOT_EXPORTED));
            }
            h.allowed(h.old, custom, 0);
            h.allowed(h.old, custom, Context.RECEIVER_NOT_EXPORTED | Context.RECEIVER_VISIBLE_TO_INSTANT_APPS);
            h.denied(h.old, custom, Context.RECEIVER_EXPORTED | Context.RECEIVER_NOT_EXPORTED, IllegalArgumentException.class);
            h.allowed(h.sharedNew, custom, 0); // Android applies the UID change to every member.
            h.allowed(h.sharedOld, custom, 0);
            rejects(SecurityException.class, () -> h.send(h.old, Intent.ACTION_BOOT_COMPLETED, h.modern.code.packageName));
            if (!binder) {
                set(h.catalog, "mFrameworkRead", false);
                h.denied(h.modern, new IntentFilter(Intent.ACTION_BOOT_COMPLETED), 0, SecurityException.class);
                set(h.catalog, "mFrameworkRead", true);
            }
            Receiver hidden = new Receiver(), exported = new Receiver(), legacy = new Receiver();
            h.register(h.modern, hidden, new IntentFilter(ACTION + ".private"), Context.RECEIVER_NOT_EXPORTED);
            h.register(h.modern, exported, new IntentFilter(ACTION + ".public"), Context.RECEIVER_EXPORTED);
            h.register(h.old, legacy, new IntentFilter(ACTION + ".legacy"), 0);
            int modernBefore = h.modern.thread.deliveries;
            h.send(h.old, ACTION + ".private", h.modern.code.packageName);
            check(h.modern.thread.deliveries == modernBefore, "Non-exported receiver accepted a different app");
            h.send(h.modern, ACTION + ".private", h.modern.code.packageName);
            check(h.modern.thread.deliveries == modernBefore + 1, "Non-exported receiver lost its own app broadcast");
            h.send(h.old, ACTION + ".public", h.modern.code.packageName);
            check(h.modern.thread.deliveries == modernBefore + 2, "Exported receiver lost an allowed broadcast");
            int oldBefore = h.old.thread.deliveries;
            h.send(h.modern, ACTION + ".legacy", h.old.code.packageName);
            check(h.old.thread.deliveries == oldBefore + 1, "Legacy implicit export was not retained");
            h.modern.call(() -> { h.am.unregisterReceiver(hidden); h.am.unregisterReceiver(exported); });
            h.old.call(() -> h.am.unregisterReceiver(legacy));
            check(h.registrations() == 0, "Finished fixture left registered receiver state");

            Receiver shared = new Receiver(), guarded = new Receiver();
            h.register(h.sharedNew, shared, new IntentFilter(ACTION + ".shared"), Context.RECEIVER_NOT_EXPORTED);
            int sharedBefore = h.sharedNew.thread.deliveries;
            h.send(h.sharedOld, ACTION + ".shared", h.sharedNew.code.packageName);
            check(h.sharedNew.thread.deliveries == sharedBefore + 1, "Non-exported receiver lost a same-UID broadcast");
            h.send(h.modern, ACTION + ".shared", h.sharedNew.code.packageName);
            check(h.sharedNew.thread.deliveries == sharedBefore + 1, "Non-exported receiver admitted an unrelated UID");
            h.register(h.modern, guarded, new IntentFilter(ACTION + ".guarded"), Context.RECEIVER_EXPORTED,
                    "android.permission.INTERNET");
            modernBefore = h.modern.thread.deliveries;
            h.send(h.modern, ACTION + ".guarded", h.modern.code.packageName);
            check(h.modern.thread.deliveries == modernBefore, "Own-package send bypassed the registered sender permission");
            h.send(h.sharedNew, ACTION + ".guarded", h.modern.code.packageName);
            check(h.modern.thread.deliveries == modernBefore + 1, "Sender permission did not include a shared-UID grant");
            h.sharedNew.call(() -> h.am.unregisterReceiver(shared));
            h.modern.call(() -> h.am.unregisterReceiver(guarded));
            check(h.registrations() == 0, "Shared-UID checks leaked receivers");

            int before = h.registrations();
            rejects(SecurityException.class, () -> h.modern.call(() -> h.am.registerReceiverWithFeature(
                    h.old.thread, h.modern.code.packageName, null, null, new Receiver(), custom,
                    null, 0, Context.RECEIVER_EXPORTED)));
            rejects(SecurityException.class, () -> h.modern.call(() -> h.am.registerReceiverWithFeature(
                    h.old.thread, h.modern.code.packageName, null, null, new Receiver(), custom,
                    null, 0, Context.RECEIVER_EXPORTED | Context.RECEIVER_NOT_EXPORTED)));
            rejects(SecurityException.class, () -> as(h.old.code.uid(), h.modern.pid, () ->
                    h.am.registerReceiverWithFeature(h.modern.thread, h.modern.code.packageName,
                    null, null, new Receiver(), custom, null, 0, Context.RECEIVER_EXPORTED)));
            check(h.registrations() == before, "Mismatched caller identity allocated receiver state");
            if (!binder) {
                // Direct host boundary: retire the enrolled process between authentication
                // and publication, without adding a production policy override or test hook.
                h.latest.thread.beforeBinder = () -> {
                    try {
                        synchronized (h.atm) {
                            ((List<?>)get(h.atm, "mProcesses")).remove(h.latest.process);
                            set(h.latest.process, "dead", true);
                        }
                        var died = h.controller.getClass().getDeclaredMethod("processDied", h.latest.process.getClass());
                        died.setAccessible(true); died.invoke(h.controller, h.latest.process);
                    } catch (Exception failure) { throw new RuntimeException(failure); }
                };
                h.register(h.latest, new Receiver(), custom, Context.RECEIVER_EXPORTED);
                check(h.registrations() == before, "Retired process acquired receiver state after cleanup");
            }
        }
        return checks;
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        System.out.println("ReceiverExportContractTest passed: " + run(true) + " checks; binder=true");
    }
}
