/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.app.ContentProviderHolder;
import android.app.Notification;
import android.Manifest;
import android.content.ComponentName;
import android.content.ContextWrapper;
import android.content.Context;
import android.content.IContentProvider;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.IShortcutService;
import android.content.pm.ParceledListSlice;
import android.content.pm.ShortcutInfo;
import android.content.pm.Signature;
import android.content.pm.PermissionInfo;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.LongSupplier;
import java.util.function.Supplier;
import sun.misc.Unsafe;

/** Actual shortcut service, policy, activity ownership and disk publication; no substitute quota. */
public final class ShortcutQuotaContractTest {
    private static final String OWNER = "fixture.shortcut.quota", PEER = OWNER + ".peer";
    private static final int UID = 12131, PID = 621131;
    private static final long DAY = 86_400_000L, NOW = 1_800_000_000_000L;
    private static final Unsafe UNSAFE;
    private static int checks;
    private static boolean cacheTransportIsolated;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe) f.get(null); }
        catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Action { void run() throws Exception; }
    private static void system(Action action) throws Exception {
        long previous = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) android.os.Process.SYSTEM_UID << 32) | android.os.Process.myPid());
            action.run();
        } finally { Binder.restoreCallingIdentity(previous); }
    }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static Class<?> type(String name) throws Exception {
        return Class.forName("com.gesher.server." + name);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        for (Class<?> at = type; at != null; at = at.getSuperclass()) {
            try { Field field = at.getDeclaredField(name); field.setAccessible(true); return field; }
            catch (NoSuchFieldException ignored) { }
        }
        throw new NoSuchFieldException(name);
    }
    private static Object get(Object object, String name) throws Exception { return field(object.getClass(), name).get(object); }
    private static void set(Object object, String name, Object value) throws Exception { field(object.getClass(), name).set(object, value); }
    private static Object construct(Class<?> type, Class<?>[] parameters, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(parameters); constructor.setAccessible(true);
        try { return constructor.newInstance(values); }
        catch (InvocationTargetException failure) { throw cause(failure); }
    }
    private static Exception cause(InvocationTargetException failure) {
        Throwable cause = failure.getCause();
        if (cause instanceof Error error) throw error;
        return cause instanceof Exception exception ? exception : new Exception(cause);
    }
    private static Object call(Object object, String name, Class<?>[] parameters, Object... values) throws Exception {
        Method method = object.getClass().getDeclaredMethod(name, parameters); method.setAccessible(true);
        try { return method.invoke(object, values); }
        catch (InvocationTargetException failure) { throw cause(failure); }
    }
    private static void rejects(Class<? extends Throwable> expected, Action action, String message) throws Exception {
        try { action.run(); throw new AssertionError(message + " accepted"); }
        catch (Throwable failure) {
            if (!expected.isInstance(failure)) throw new AssertionError(message + ": " + failure, failure);
            checks++;
        }
    }
    private static ApkPackage code(String name, long installed) throws Exception {
        return code(name, installed, new String[0]);
    }
    private static ApkPackage code(String name, long installed, String... permissions) throws Exception {
        return code(name, installed, UID, permissions);
    }
    private static ApkPackage code(String name, long installed, int uid, String... permissions) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/shortcut/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(28);
        if (!name.equals("android") && uid == UID) parsed.setSharedUserId("fixture.shortcut.shared");
        parsed.addUsesPermission(new ParsedUsesPermissionImpl(Manifest.permission.FOREGROUND_SERVICE, 0, Set.of()));
        for (String permission : permissions) parsed.addUsesPermission(new ParsedUsesPermissionImpl(permission, 0, Set.of()));
        ParsedActivityImpl activity = new ParsedActivityImpl();
        activity.setPackageName(name); activity.setName(name + ".Main"); activity.setEnabled(true);
        parsed.addActivity(activity);
        ParsedServiceImpl service = new ParsedServiceImpl();
        service.setPackageName(name); service.setName(name + ".Foreground"); service.setEnabled(true);
        service.setProcessName(name); service.setExported(true); parsed.addService(service);
        ParsedProviderImpl provider = new ParsedProviderImpl();
        provider.setPackageName(name); provider.setName(name + ".Provider");
        provider.setProcessName(name); provider.setAuthority(name + ".authority");
        provider.setEnabled(true); provider.setExported(true); parsed.addProvider(provider);
        parsed.hideAsFinal();
        ApkPackage apk = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        set(apk, "packageName", name); set(apk, "parsed", parsed);
        set(apk, "mUid", name.equals("android") ? android.os.Process.SYSTEM_UID : uid);
        set(apk, "apkPath", "/fixture/shortcut/base.apk");
        set(apk, "mDataDir", System.getenv("ANDROID_DATA") + "/data/" + name);
        set(apk, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        set(apk, "mSigningIdentityChecked", true);
        set(apk, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{4, 5, 6})});
        apk.setInstallTimes(installed, installed);
        return apk;
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    private static Object state(String name) throws Exception {
        return Enum.valueOf((Class) type("ActivityTaskManagerService$State"), name);
    }
    private static ShortcutInfo.Builder builder(String name, String id) {
        return new ShortcutInfo.Builder(new ContextWrapper(null) {
            @Override public String getPackageName() { return name; }
            @Override public int getUserId() { return 0; }
        }, id);
    }
    private static ParceledListSlice<ShortcutInfo> slice(ShortcutInfo... values) {
        return new ParceledListSlice<>(Arrays.asList(values));
    }
    private static IShortcutService transported(Object implementation, boolean parcel) throws Exception {
        IShortcutService service = (IShortcutService) implementation;
        if (!parcel) return service;
        Class<?> standIn = type("StandIn");
        standIn.getMethod("setStrict", boolean.class).invoke(null, true);
        Object wrapper = standIn.getMethod("of", Class.class, IInterface.class).invoke(null, IShortcutService.class, service);
        IBinder target = (IBinder) standIn.getMethod("asBinder").invoke(wrapper);
        return IShortcutService.Stub.asInterface(new Binder() {
            @Override public IInterface queryLocalInterface(String name) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, data, reply, flags);
            }
        });
    }
    private static final class Harness {
        final ApkPackage owner = code(OWNER, 1234), peer = code(PEER, 5678);
        final PackageManagerService pm;
        final Object atm, am, process, callers, notifications;
        final AtomicLong clock = new AtomicLong(NOW);
        final AtomicReference<String> locale = new AtomicReference<>("en-US");
        final boolean parcel;
        Object implementation;
        IShortcutService api;
        @SuppressWarnings("unchecked")
        Harness(boolean parcel, Path state, boolean defaults) throws Exception {
            this.parcel = parcel;
            pm = (PackageManagerService) construct(PackageManagerService.class,
                    new Class<?>[]{java.util.Collection.class, boolean.class}, List.of(owner, peer), defaults);
            set(pm, "mPackageEventSender", (java.util.function.Consumer<Intent>) event -> { });
            atm = construct(type("ActivityTaskManagerService"), new Class<?>[]{PackageManagerService.class}, pm);
            notifications = construct(type("NotificationManagerService"), new Class<?>[]{PackageManagerService.class,
                    type("ActivityManagerService"), java.util.function.ToIntBiFunction.class}, pm, null,
                    (java.util.function.ToIntBiFunction<android.service.notification.StatusBarNotification, Boolean>)
                            (notification, update) -> -1); // no native toast transport in this isolated controller
            am = construct(type("ActivityManagerService"), new Class<?>[]{atm.getClass(), PackageManagerService.class,
                    notifications.getClass()}, atm, pm, notifications);
            callers = call(am, "callers", new Class<?>[0]);
            set(notifications, "mActivityManager", am);
            if (!parcel) {
                // Exact Android manifest definitions; host has no framework-res AssetManager.
                Object catalog = get(pm.permissions(), "mCatalog");
                Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>) get(catalog, "mFramework");
                for (String name : List.of(Manifest.permission.FOREGROUND_SERVICE,
                        Manifest.permission.UNLIMITED_SHORTCUTS_API_CALLS,
                        Manifest.permission.RESET_SHORTCUT_MANAGER_THROTTLING)) {
                    PermissionInfo info = new PermissionInfo(); info.name = name; info.packageName = "android";
                    info.protectionLevel = name.equals(Manifest.permission.FOREGROUND_SERVICE)
                            ? PermissionInfo.PROTECTION_NORMAL | PermissionInfo.PROTECTION_FLAG_INSTANT
                            : PermissionInfo.PROTECTION_SIGNATURE;
                    if (name.equals(Manifest.permission.UNLIMITED_SHORTCUTS_API_CALLS)) {
                        info.protectionLevel |= PermissionInfo.PROTECTION_FLAG_ROLE;
                    }
                    definitions.put(name, info);
                }
                set(catalog, "mLoaded", true);
                call(pm.permissions(), "initializePackages", new Class<?>[0]);
            }
            IApplicationThread thread = new IApplicationThread.Default() {
                final IBinder binder = new Binder();
                @Override public IBinder asBinder() { return binder; }
            };
            long original = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) UID << 32) | PID);
                process = construct(type("ActivityTaskManagerService$ProcessRecord"),
                        new Class<?>[]{atm.getClass(), int.class, IApplicationThread.class}, atm, PID, thread);
            } finally { Binder.restoreCallingIdentity(original); }
            set(process, "packageName", OWNER); set(process, "processName", OWNER);
            set(process, "packageCode", owner); set(process, "ready", true);
            ((List<Object>) get(atm, "mProcesses")).add(process);
            reopen(state, defaults);
        }
        void reopen(Path state, boolean defaults) throws Exception {
            Class<?> service = type("DesktopShortcutService");
            implementation = defaults ? construct(service, new Class<?>[]{PackageManagerService.class, callers.getClass()}, pm, callers)
                    : construct(service, new Class<?>[]{PackageManagerService.class, callers.getClass(), Path.class,
                            LongSupplier.class, Supplier.class}, pm, callers, state,
                            (LongSupplier) clock::get, (Supplier<String>) locale::get);
            api = transported(implementation, parcel);
        }
        void app(Action action) throws Exception {
            long original = Binder.clearCallingIdentity();
            try { Binder.restoreCallingIdentity(((long) UID << 32) | PID); action.run(); }
            finally { Binder.restoreCallingIdentity(original); }
        }
        int remaining(String name) throws Exception {
            int[] value = new int[1]; app(() -> value[0] = api.getRemainingCallCount(name, 0)); return value[0];
        }
        void exhaust(String name) throws Exception {
            app(() -> { for (int i = 0; i < 10; i++) check(api.setDynamicShortcuts(name, ParceledListSlice.emptyList(), 0), "quota denied call " + i); });
            check(remaining(name) == 0, "quota did not reach zero");
        }
        void flush() throws Exception { check((Boolean) call(implementation, "flushQuota", new Class<?>[0]), "quota write failed"); }
        void manualState(String name) throws Exception {
            call(atm, "inProcessActivityState", new Class<?>[]{process.getClass(), type("ActivityTaskManagerService$State")},
                    process, state(name));
        }
        @SuppressWarnings("unchecked")
        Object activity() throws Exception {
            ActivityInfo info = owner.activityInfo(OWNER + ".Main", 0);
            Object record = construct(type("ActivityTaskManagerService$ActivityRecord"),
                    new Class<?>[]{atm.getClass(), ActivityInfo.class, Intent.class, int.class, int.class,
                            String.class, type("ActivityTaskManagerService$ActivityRecord"), String.class, int.class},
                    atm, info, new Intent().setComponent(new ComponentName(OWNER, info.name)), 1, PID, OWNER, null, null, 0);
            set(record, "app", process); set(record, "state", state("RESUMING"));
            ((List<Object>) get(process, "activities")).add(record);
            ((List<Object>) get(atm, "mActivities")).add(record);
            return record;
        }
        @SuppressWarnings("unchecked")
        Object service() throws Exception {
            return service(process);
        }
        @SuppressWarnings("unchecked")
        Object service(Object process) throws Exception {
            ApkPackage owner = (ApkPackage) get(process, "packageCode");
            ServiceInfo info = owner.serviceInfo(owner.packageName + ".Foreground", 0);
            Object record = construct(type("ActiveServices$ServiceRecord"),
                    new Class<?>[]{ServiceInfo.class, ApkPackage.class}, info, owner);
            set(record, "app", process); set(record, "created", true); set(record, "startRequested", true);
            Object services = get(am, "mServices");
            ((Map<ComponentName, Object>) get(services, "mServices")).put(new ComponentName(owner.packageName, info.name), record);
            return record;
        }
        void foregroundService(Object record, int id) throws Exception {
            Object services = get(am, "mServices");
            app(() -> call(services, "setServiceForeground", new Class<?>[]{ComponentName.class, IBinder.class,
                    int.class, Notification.class, int.class, int.class},
                    new ComponentName(OWNER, OWNER + ".Foreground"), record, id, id == 0 ? null : new Notification(), 0, 0));
        }
        @SuppressWarnings("unchecked")
        Object extra(String name, int uid, int pid) throws Exception {
            ApkPackage owner = code(name, 2345, uid);
            call(pm, "add", new Class<?>[]{ApkPackage.class}, owner);
            IApplicationThread thread = new IApplicationThread.Default() {
                final IBinder binder = new Binder();
                @Override public IBinder asBinder() { return binder; }
            };
            Object[] result = new Object[1];
            long original = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) uid << 32) | pid);
                result[0] = construct(process.getClass(), new Class<?>[]{atm.getClass(), int.class,
                        IApplicationThread.class}, atm, pid, thread);
            } finally { Binder.restoreCallingIdentity(original); }
            set(result[0], "packageName", name); set(result[0], "processName", name);
            set(result[0], "packageCode", owner); set(result[0], "ready", true);
            set(result[0], "inProcessActivityState", state("STOPPED"));
            ((List<Object>) get(atm, "mProcesses")).add(result[0]);
            return result[0];
        }
        int remainingFor(Object process) throws Exception {
            int[] value = new int[1];
            as(process, () -> value[0] = api.getRemainingCallCount((String) get(process, "packageName"), 0));
            return value[0];
        }
        void exhaustFor(Object process) throws Exception {
            as(process, () -> { for (int i = 0; i < 10; i++) check(api.setDynamicShortcuts(
                    (String) get(process, "packageName"), ParceledListSlice.emptyList(), 0), "dependent budget call " + i); });
            check(remainingFor(process) == 0, "dependent quota did not reach zero");
        }
        void stateFor(Object process, String value) throws Exception {
            call(atm, "inProcessActivityState", new Class<?>[]{process.getClass(), type("ActivityTaskManagerService$State")},
                    process, state(value));
        }
        IServiceConnection bind(Object client, Object target, long flags) throws Exception {
            return bind(client, target, flags, client == null ? null : (IApplicationThread) get(client, "thread"),
                    client == null ? "android" : (String) get(client, "packageName"));
        }
        IServiceConnection bind(Object client, Object target, long flags, IApplicationThread thread,
                String claimed) throws Exception {
            IServiceConnection connection = new IServiceConnection.Default() {
                final IBinder binder = new Binder();
                @Override public IBinder asBinder() { return binder; }
            };
            Action bind = () -> check((Integer) call(am, "bindService", new Class<?>[]{IApplicationThread.class,
                    IBinder.class, Intent.class, String.class, IServiceConnection.class, long.class, String.class, int.class},
                    thread, null,
                    new Intent().setComponent(new ComponentName((String) get(target, "packageName"),
                            get(target, "packageName") + ".Foreground")), null, connection, flags,
                    claimed, 0) == 1, "real binding was not admitted");
            if (client == null) system(bind); else as(client, bind);
            return connection;
        }
        void unbind(Object client, IServiceConnection connection) throws Exception {
            Action unbind = () -> check((Boolean) call(am, "unbindService", new Class<?>[]{IServiceConnection.class},
                    connection), "owned connection was not removed");
            if (client == null) system(unbind); else as(client, unbind);
        }
        void publishProvider(Object process) throws Exception {
            Object providers = get(am, "mProviders");
            ApkPackage code = (ApkPackage) get(process, "packageCode");
            var info = code.providers(0).get(0);
            call(providers, "providersFor", new Class<?>[]{process.getClass(), android.content.pm.ApplicationInfo.class},
                    process, info.applicationInfo);
            ContentProviderHolder holder = new ContentProviderHolder(info);
            IBinder binder = new Binder();
            holder.provider = (IContentProvider) java.lang.reflect.Proxy.newProxyInstance(
                    IContentProvider.class.getClassLoader(), new Class<?>[]{IContentProvider.class},
                    (proxy, method, args) -> {
                        if (method.getName().equals("asBinder")) return binder;
                        throw new AssertionError("Unexpected provider operation " + method.getName());
                    });
            as(process, () -> call(am, "publishContentProviders", new Class<?>[]{IApplicationThread.class, List.class},
                    get(process, "thread"), List.of(holder)));
        }
        ContentProviderHolder acquire(Object client, Object target, boolean stable) throws Exception {
            ContentProviderHolder[] result = new ContentProviderHolder[1];
            as(client, () -> result[0] = (ContentProviderHolder) call(am, "getContentProvider",
                    new Class<?>[]{IApplicationThread.class, String.class, String.class, int.class, boolean.class},
                    get(client, "thread"), get(client, "packageName"), get(target, "packageName") + ".authority", 0, stable));
            check(result[0] != null && result[0].connection != null, "real counted provider acquisition missing");
            return result[0];
        }
        void release(Object client, ContentProviderHolder holder, boolean stable) throws Exception {
            as(client, () -> call(am, "removeContentProvider", new Class<?>[]{IBinder.class, boolean.class},
                    holder.connection, stable));
        }
    }

    private static void as(Object process, Action action) throws Exception {
        long original = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) (Integer) get(process, "uid") << 32) | (Integer) get(process, "pid"));
            action.run();
        } finally { Binder.restoreCallingIdentity(original); }
    }

    private static void basics(boolean parcel, Path file) throws Exception {
        Harness h = new Harness(parcel, file, false);
        check(h.remaining(OWNER) == 10 && h.remaining(PEER) == 10, "initial package quota");
        h.exhaust(OWNER);
        h.app(() -> {
            check(!h.api.setDynamicShortcuts(OWNER, ParceledListSlice.emptyList(), 0), "set exceeded quota");
            check(!h.api.addDynamicShortcuts(OWNER, ParceledListSlice.emptyList(), 0), "add exceeded quota");
            check(!h.api.updateShortcuts(OWNER, ParceledListSlice.emptyList(), 0), "update exceeded quota");
            rejects(SecurityException.class, () -> h.api.getRemainingCallCount("foreign", 0), "foreign publisher");
            rejects(SecurityException.class, h.api::resetThrottling, "app global reset");
            rejects(SecurityException.class, () -> h.api.onApplicationActive(OWNER, 0), "app targeted reset");
            h.api.removeAllDynamicShortcuts(OWNER, 0);
            check(h.api.getRemainingCallCount(OWNER, 0) == 0, "removal reset the quota");
            check(h.api.getRemainingCallCount(PEER, 0) == 10, "shared UID merged per-package quota");
            check(h.api.getRateLimitResetTime(OWNER, 0) == NOW + DAY, "wrong reset anchor");
        });
        h.flush();
        h.reopen(file, false);
        check(h.remaining(OWNER) == 0, "quota disappeared on reopen");
        h.clock.addAndGet(DAY);
        check(h.remaining(OWNER) == 10, "daily reset missing");
        h.exhaust(OWNER);
        h.clock.addAndGet(-DAY / 2);
        check(h.remaining(OWNER) == 10, "valid clock rewind did not reset");
        h.exhaust(OWNER);
        h.locale.set("he-IL");
        check(h.remaining(OWNER) == 10, "locale change did not reset");
        h.exhaust(OWNER);
        system(() -> h.api.onApplicationActive(OWNER, 0));
        check(h.remaining(OWNER) == 10, "authorized targeted reset failed");
        h.exhaust(OWNER);
        system(h.api::resetThrottling);
        check(h.remaining(OWNER) == 10, "authorized global reset failed");
        h.flush();
    }

    private static void validation(boolean parcel) throws Exception {
        Harness h = new Harness(parcel, null, false);
        ShortcutInfo incomplete = builder(OWNER, "missing-intent").setShortLabel("incomplete").build();
        h.app(() -> {
            rejects(SecurityException.class, () -> h.api.setDynamicShortcuts(OWNER,
                    slice(builder("foreign", "id").build()), 0), "payload publisher precedes quota");
            rejects(IllegalArgumentException.class, () -> h.api.setDynamicShortcuts(OWNER,
                    new ParceledListSlice<>(java.util.Collections.nCopies(16, incomplete)), 0), "oversized input precedes quota");
            rejects(NullPointerException.class, () -> h.api.setDynamicShortcuts(OWNER, slice((ShortcutInfo) null), 0), "null entry");
            check(h.api.getRemainingCallCount(OWNER, 0) == 10, "early validation consumed quota");
            rejects(IllegalArgumentException.class, () -> h.api.setDynamicShortcuts(OWNER, slice(incomplete), 0), "late validation");
            check(h.api.getRemainingCallCount(OWNER, 0) == 9, "late validation did not consume quota");
            for (int i = 0; i < 9; i++) check(h.api.updateShortcuts(OWNER,
                    slice(builder(OWNER, "unknown").build()), 0), "partial unknown update rejected before quota exhausted");
            check(h.api.getRemainingCallCount(OWNER, 0) == 0, "unknown updates did not consume quota");
            check(!h.api.setDynamicShortcuts(OWNER, slice(incomplete), 0), "late invalid payload bypassed throttling");
            check(!h.api.addDynamicShortcuts(OWNER, slice(incomplete), 0), "late invalid add bypassed throttling");
            check(!h.api.updateShortcuts(OWNER, slice(incomplete), 0), "late invalid update bypassed throttling");
            rejects(SecurityException.class, () -> h.api.updateShortcuts(OWNER,
                    slice(builder("foreign", "id").build()), 0), "throttled payload identity");
            h.api.disableShortcuts(OWNER, List.of("unknown"), null, 0, 0);
            h.api.enableShortcuts(OWNER, List.of("unknown"), 0);
            h.api.removeDynamicShortcuts(OWNER, List.of("unknown"), 0);
            check(h.api.getRemainingCallCount(OWNER, 0) == 0, "unlimited removal/state methods reset quota");
        });
        h.flush();
    }

    @SuppressWarnings("unchecked")
    private static void foreground(boolean parcel) throws Exception {
        Harness h = new Harness(parcel, null, false);
        h.exhaust(OWNER); h.exhaust(PEER);
        Object activity = h.activity();
        call(h.atm, "activityResumed", new Class<?>[]{activity.getClass()}, activity);
        set(activity, "state", state("STOPPED"));
        check(h.remaining(OWNER) == 10 && h.remaining(PEER) == 10,
                "real activity resume between calls did not reset the UID's package counters");
        h.exhaust(OWNER); h.exhaust(PEER);
        set(activity, "state", state("RESUMED"));
        h.app(() -> {
            for (int i = 0; i < 24; i++) check(h.api.setDynamicShortcuts(OWNER, ParceledListSlice.emptyList(), 0),
                    "current activity foreground was throttled");
            check(h.api.getRemainingCallCount(OWNER, 0) == 10, "foreground query did not reset its counter");
        });
        // OomAdjuster: an activity paused (or started) and still visible under a
        // translucent activity in its task keeps its process TOP. The translucent
        // activity belongs to another app, so only the covered one can exempt OWNER.
        Object other = h.extra("fixture.shortcut.translucent", UID + 7, PID + 7);
        Object cover = h.activity();
        ((List<Object>) get(h.process, "activities")).remove(cover);
        set(cover, "app", other); ((List<Object>) get(other, "activities")).add(cover);
        set(cover, "translucent", true); set(cover, "state", state("RESUMED"));
        for (String covered : List.of("PAUSED", "STARTED")) {
            set(activity, "state", state(covered));
            h.app(() -> {
                for (int i = 0; i < 11; i++) check(h.api.setDynamicShortcuts(OWNER, ParceledListSlice.emptyList(), 0),
                        "visible activity under a translucent one was throttled: " + covered);
                check(h.api.getRemainingCallCount(OWNER, 0) == 10, "visible covered activity lost TOP: " + covered);
            });
        }
        set(activity, "state", state("STOPPED")); h.exhaust(OWNER);
        set(cover, "translucent", false); set(activity, "state", state("STARTED")); h.exhaust(OWNER);
        ((List<Object>) get(other, "activities")).clear();
        ((List<Object>) get(h.atm, "mActivities")).remove(cover);
        ((List<Object>) get(h.atm, "mProcesses")).remove(other);
        set(activity, "state", state("STOPPED"));
        ((List<Object>) get(h.process, "activities")).clear();
        ((List<Object>) get(h.atm, "mActivities")).clear();
        h.exhaust(OWNER);
        set(h.process, "inProcessActivityState", state("STOPPED"));
        h.manualState("RESUMED"); h.manualState("STOPPED");
        check(h.remaining(OWNER) == 10 && h.remaining(PEER) == 10, "manual-driver foreground visit lost");
        h.exhaust(OWNER);
        set(h.process, "pidSuperseded", true);
        h.manualState("RESUMED");
        set(h.process, "pidSuperseded", false); h.manualState("STOPPED");
        check(h.remaining(OWNER) == 0, "superseded process reset quota");
        set(h.process, "inProcessActivityState", null);
        Object service = h.service();
        h.foregroundService(service, 91); h.foregroundService(service, 0);
        check(h.remaining(OWNER) == 10, "foreground service visit between calls lost");
        h.exhaust(OWNER);
        h.foregroundService(service, 92);
        h.app(() -> { for (int i = 0; i < 24; i++) check(h.api.setDynamicShortcuts(OWNER,
                ParceledListSlice.emptyList(), 0), "active foreground service was throttled"); });
        check(h.remaining(OWNER) == 10, "foreground service query was throttled");
        h.foregroundService(service, 0); h.exhaust(OWNER);
        check(h.remaining(OWNER) == 0, "stopped foreground service retained exemption");
        ApkPackage replacement = code(OWNER, h.owner.firstInstallTime());
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, replacement);
        set(h.process, "packageCode", replacement);
        // #260 can reject this retired token before the existing foreground
        // mutation. Both compositions must preserve the replacement's budget.
        try { h.foregroundService(service, 93); } catch (SecurityException expectedRetiredOwner) { }
        check(h.remaining(OWNER) == 0, "retired service token reset or exempted a replacement publisher");
        h.foregroundService(service, 0);
        h.flush();
    }

    @SuppressWarnings("unchecked")
    private static void dependencies(boolean parcel) throws Exception {
        Harness h = new Harness(parcel, null, false);
        set(h.process, "inProcessActivityState", state("STOPPED"));
        Object target = h.extra("fixture.shortcut.bound", UID + 1, PID + 1);
        Object next = h.extra("fixture.shortcut.transitive", UID + 2, PID + 2);
        h.service(target); h.service(next);
        h.exhaustFor(target); h.exhaustFor(next);
        IServiceConnection first = h.bind(h.process, target, Context.BIND_AUTO_CREATE);
        IServiceConnection second = h.bind(target, next, Context.BIND_AUTO_CREATE);
        IServiceConnection cycle = h.bind(next, target, Context.BIND_AUTO_CREATE);
        check(h.remainingFor(target) == 0 && h.remainingFor(next) == 0, "background dependency cycle invented foreground");
        h.manualState("RESUMED"); h.manualState("STOPPED");
        check(h.remainingFor(target) == 10 && h.remainingFor(next) == 10, "transitive bound-top visit between calls lost");
        h.exhaustFor(target); h.exhaustFor(next);
        h.manualState("RESUMED");
        as(next, () -> { for (int i = 0; i < 24; i++) check(h.api.setDynamicShortcuts(
                (String) get(next, "packageName"), ParceledListSlice.emptyList(), 0), "current transitive bound-top throttled"); });
        check(h.remainingFor(next) == 10, "current dependency did not reset allowance");
        h.manualState("STOPPED");
        h.exhaustFor(target); h.exhaustFor(next);
        check(h.remainingFor(target) == 0 && h.remainingFor(next) == 0, "cycle retained state after its only root stopped");
        Object foregroundService = h.service();
        h.foregroundService(foregroundService, 94); h.foregroundService(foregroundService, 0);
        check(h.remainingFor(target) == 10 && h.remainingFor(next) == 10, "bound-FGS visit lost");
        h.exhaustFor(target); h.exhaustFor(next);
        h.unbind(h.process, first); h.unbind(target, second); h.unbind(next, cycle);

        h.manualState("RESUMED");
        for (long flags : new long[]{Context.BIND_WAIVE_PRIORITY, Context.BIND_NOT_FOREGROUND,
                Context.BIND_IMPORTANT_BACKGROUND, Context.BIND_WAIVE_PRIORITY | Context.BIND_ADJUST_WITH_ACTIVITY}) {
            IServiceConnection denied = h.bind(h.process, target, flags);
            check(h.remainingFor(target) == 0, "restrictive binding flag fabricated exemption " + flags);
            h.unbind(h.process, denied);
        }
        for (long flags : new long[]{Context.BIND_NOT_PERCEPTIBLE, Context.BIND_NOT_VISIBLE,
                Context.BIND_ALLOW_OOM_MANAGEMENT}) {
            IServiceConnection allowed = h.bind(h.process, target, flags);
            check(h.remainingFor(target) == 10, "adj-only flag incorrectly removed process-state exemption " + flags);
            h.unbind(h.process, allowed); h.exhaustFor(target);
        }
        Object sameUidBackground = h.extra("fixture.shortcut.sameuid", UID, PID + 3);
        IServiceConnection sameUid = h.bind(sameUidBackground, target, 0);
        check(h.remainingFor(target) == 0, "foreground UID peer promoted a different background client process");
        h.stateFor(sameUidBackground, "RESUMED"); h.stateFor(sameUidBackground, "STOPPED");
        check(h.remainingFor(target) == 10, "actual shared-UID client process visit lost");
        h.unbind(sameUidBackground, sameUid); h.exhaustFor(target);
        h.stateFor(target, "RESUMED"); h.stateFor(target, "STOPPED"); h.exhaustFor(target);
        IServiceConnection shown = h.bind(h.process, target, Context.BIND_ALLOW_OOM_MANAGEMENT);
        check(h.remainingFor(target) == 0, "ALLOW_OOM_MANAGEMENT ignored actual shown-UI history");
        h.unbind(h.process, shown);

        IServiceConnection live = h.bind(h.process, target, 0);
        check(h.remainingFor(target) == 10, "ordinary binding to former UI host lost its exemption");
        h.unbind(h.process, live); h.exhaustFor(target);
        set(h.process, "pidSuperseded", true);
        rejects(SecurityException.class, () -> h.bind(h.process, target, 0), "superseded binding client");
        set(h.process, "pidSuperseded", false);
        IServiceConnection retired = h.bind(h.process, target, 0);
        check(h.remainingFor(target) == 10, "live client not exempted");
        h.manualState("STOPPED"); h.exhaustFor(target);
        set(h.process, "inProcessActivityState", state("RESUMED"));
        ((List<Object>) get(h.atm, "mProcesses")).remove(h.process);
        check(h.remainingFor(target) == 0, "detached client retained foreground through a connection");
        ((List<Object>) get(h.atm, "mProcesses")).add(h.process);
        set(h.process, "inProcessActivityState", state("STOPPED"));
        h.unbind(h.process, retired);

        for (long flags : new long[]{0, Context.BIND_NOT_FOREGROUND | Context.BIND_FOREGROUND_SERVICE,
                Context.BIND_WAIVE_PRIORITY | Context.BIND_IMPORTANT}) {
            IServiceConnection system = h.bind(null, target, flags);
            check(h.remainingFor(target) == 0, "system binding without qualifying state escaped quota " + flags);
            h.unbind(null, system);
        }
        for (long flags : new long[]{Context.BIND_FOREGROUND_SERVICE, Context.BIND_TREAT_LIKE_VISIBLE_FOREGROUND_SERVICE,
                Context.BIND_IMPORTANT | Context.BIND_NOT_FOREGROUND, Context.BIND_ABOVE_CLIENT}) {
            IServiceConnection system = h.bind(null, target, flags);
            check(h.remainingFor(target) == 10, "actual persistent-client rule lost " + flags);
            h.unbind(null, system); h.exhaustFor(target);
        }
        IServiceConnection persistent = h.bind(null, target, Context.BIND_IMPORTANT);
        IServiceConnection belowPersistent = h.bind(target, next, Context.BIND_IMPORTANT);
        check(h.remainingFor(next) == 0, "bound persistent host retransmitted system-server-only adj exception");
        h.unbind(target, belowPersistent);
        IServiceConnection persistentForeground = h.bind(target, next, Context.BIND_FOREGROUND_SERVICE);
        check(h.remainingFor(next) == 10, "persistent-host foreground-service rule lost");
        h.unbind(target, persistentForeground); h.exhaustFor(next);
        h.unbind(null, persistent); h.exhaustFor(target);
        AtomicReference<Boolean> awake = new AtomicReference<>(false);
        call(h.am, "attachShortcutPower", new Class<?>[]{java.util.function.BooleanSupplier.class},
                (java.util.function.BooleanSupplier) awake::get);
        IServiceConnection conditional = h.bind(null, target, Context.BIND_FOREGROUND_SERVICE_WHILE_AWAKE);
        check(h.remainingFor(target) == 0, "asleep conditional binding escaped quota");
        awake.set(true); call(h.am, "refreshShortcutForeground", new Class<?>[0]); awake.set(false);
        check(h.remainingFor(target) == 10, "actual awake transition visit lost");
        h.unbind(null, conditional); h.exhaustFor(target);

        Object local = h.extra("fixture.shortcut.local", UID + 6, android.os.Process.myPid());
        h.stateFor(local, "RESUMED");
        IServiceConnection localBinding = h.bind(local, target, 0);
        check(h.remainingFor(target) == 10, "admitted standalone client lost its real bound-top dependency");
        h.unbind(local, localBinding); h.exhaustFor(target);
        IServiceConnection noThread = h.bind(local, target, 0, null, (String) get(local, "packageName"));
        check(h.remainingFor(target) == 0, "trusted package claim without application thread created a root");
        h.unbind(local, noThread);
        IServiceConnection otherClaim = h.bind(local, target, 0, (IApplicationThread) get(local, "thread"), OWNER);
        check(h.remainingFor(target) == 0, "trusted package delegation borrowed a different process's state");
        h.unbind(local, otherClaim);
        h.flush();
    }

    private static void evaluationCleared(Object am) throws Exception {
        Object scratch = get(am, "mShortcutEvaluation");
        check(((Map<?, ?>) get(scratch, "states")).isEmpty(), "evaluation retained process-state authority");
        check(((Map<?, ?>) get(scratch, "members")).isEmpty(), "evaluation retained process membership");
        check(((java.util.Collection<?>) get(scratch, "work")).isEmpty(), "evaluation retained queued process references");
        check(get(scratch, "processes") == null && get(scratch, "codeCurrent") == null
                && !(Boolean) get(scratch, "inUse"), "evaluation retained registry, predicate, or active ownership");
    }

    private static void powerVisits(boolean parcel) throws Exception {
        Harness h = new Harness(parcel, null, false);
        Object target = h.extra("fixture.shortcut.awake", UID + 8, PID + 8);
        h.service(target); h.exhaustFor(target);
        Object power = construct(type("PowerManagerService"), new Class<?>[]{h.am.getClass()}, h.am);
        int suspend = field(type("WindowsHost"), "EVENT_SUSPENDING").getInt(null);
        int resume = field(type("WindowsHost"), "EVENT_RESUMED").getInt(null);
        for (boolean sameBatch : new boolean[]{false, true}) {
            IServiceConnection binding = h.bind(null, target, Context.BIND_FOREGROUND_SERVICE_WHILE_AWAKE);
            as(target, () -> check(h.api.setDynamicShortcuts((String) get(target, "packageName"),
                    ParceledListSlice.emptyList(), 0), "awake-bound publication failed"));
            // Real PowerManager event-handler transitions, with no shortcut
            // query while asleep, and final unbind before the next query.
            if (sameBatch) call(power, "onWindowsEvents", new Class<?>[]{int.class}, suspend | resume);
            else {
                call(power, "onWindowsEvents", new Class<?>[]{int.class}, suspend);
                call(power, "onWindowsEvents", new Class<?>[]{int.class}, resume);
            }
            h.unbind(null, binding);
            check(h.remainingFor(target) == 10, "sleep/resume lost a real qualifying visit: sameBatch=" + sameBatch);
            h.exhaustFor(target);
        }
        IServiceConnection unchanged = h.bind(null, target, Context.BIND_FOREGROUND_SERVICE_WHILE_AWAKE);
        as(target, () -> check(h.api.setDynamicShortcuts((String) get(target, "packageName"),
                ParceledListSlice.emptyList(), 0), "unchanged-awake publication failed"));
        call(power, "onWindowsEvents", new Class<?>[]{int.class}, resume);
        h.unbind(null, unchanged);
        check(h.remainingFor(target) == 9, "duplicate resume event manufactured a foreground visit");
        h.flush();
    }

    @SuppressWarnings("unchecked")
    private static void evaluationFailure(boolean parcel) throws Exception {
        Harness h = new Harness(parcel, null, false);
        set(h.process, "inProcessActivityState", state("STOPPED"));
        Object target = h.extra("fixture.shortcut.failure", UID + 7, PID + 7);
        h.service(target); h.exhaustFor(target);
        IServiceConnection binding = h.bind(h.process, target, 0);
        h.manualState("RESUMED");
        check(h.remainingFor(target) == 10, "evaluation precondition missing bound foreground");
        evaluationCleared(h.am);
        Object services = get(h.am, "mServices");
        Object record = ((Map<IBinder, List<Object>>) get(services, "mConnections")).get(binding.asBinder()).get(0);
        Object lifetime = get(record, "lifetime");
        set(record, "lifetime", (java.util.function.BooleanSupplier) () -> {
            throw new IllegalStateException("controlled dependency failure");
        });
        synchronized (services) { call(services, "publishShortcutStateLocked", new Class<?>[0]); }
        rejects(IllegalStateException.class, () -> h.remainingFor(target), "failed dependency evaluation");
        evaluationCleared(h.am);
        set(record, "lifetime", lifetime);
        synchronized (services) { call(services, "publishShortcutStateLocked", new Class<?>[0]); }
        check(h.remainingFor(target) == 10, "failed evaluation poisoned the reusable work owner");
        h.unbind(h.process, binding); h.exhaustFor(target);
        check(h.remainingFor(target) == 0, "successful prior evaluation escaped final connection retirement");
        evaluationCleared(h.am); h.flush();
    }

    private static void providers(boolean parcel) throws Exception {
        Harness h = new Harness(parcel, null, false);
        set(h.process, "inProcessActivityState", state("STOPPED"));
        Object provider = h.extra("fixture.shortcut.provider", UID + 4, PID + 4);
        Object dependent = h.extra("fixture.shortcut.provider.bound", UID + 5, PID + 5);
        h.publishProvider(provider); h.service(dependent);
        h.exhaustFor(provider); h.exhaustFor(dependent);
        IServiceConnection binding = h.bind(provider, dependent, 0);
        ContentProviderHolder stable = h.acquire(h.process, provider, true);
        check(h.remainingFor(provider) == 0, "background provider acquisition invented foreground");
        h.manualState("RESUMED"); h.manualState("STOPPED");
        check(h.remainingFor(provider) == 10 && h.remainingFor(dependent) == 10,
                "provider-to-service foreground visit lost");
        h.exhaustFor(provider); h.exhaustFor(dependent);
        Object foreground = h.service();
        h.foregroundService(foreground, 95); h.foregroundService(foreground, 0);
        check(h.remainingFor(provider) == 10 && h.remainingFor(dependent) == 10,
                "provider bound-FGS transition lost");
        h.exhaustFor(provider); h.exhaustFor(dependent);
        ContentProviderHolder unstable = h.acquire(h.process, provider, false);
        h.release(h.process, stable, true);
        h.manualState("RESUMED");
        check(h.remainingFor(provider) == 10, "live unstable count lost foreground dependency");
        as(provider, () -> check(h.api.setDynamicShortcuts((String) get(provider, "packageName"),
                ParceledListSlice.emptyList(), 0), "foreground provider publication failed"));
        // Repeating a topology notification without changing this UID's state
        // must not manufacture another foreground visit after its last call.
        IServiceConnection unrelated = h.bind(h.process, dependent, Context.BIND_WAIVE_PRIORITY);
        h.unbind(h.process, unrelated);
        h.release(h.process, unstable, false);
        check(h.remainingFor(provider) == 9, "unchanged observer state refunded the last foreground publication");
        as(provider, () -> { for (int i = 0; i < 9; i++) check(h.api.setDynamicShortcuts(
                (String) get(provider, "packageName"), ParceledListSlice.emptyList(), 0), "remaining provider budget lost"); });
        check(h.remainingFor(provider) == 0, "last provider reference retained an exemption");
        h.unbind(provider, binding);
        ContentProviderHolder fresh = h.acquire(h.process, provider, true);
        check(h.remainingFor(provider) == 10, "new reference to foreground client did not reset");
        h.manualState("STOPPED"); h.exhaustFor(provider);
        h.manualState("RESUMED");
        ApkPackage original = (ApkPackage) get(provider, "packageCode");
        ApkPackage replacement = code(original.packageName, original.firstInstallTime(), original.uid());
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, replacement);
        set(provider, "packageCode", replacement);
        // Consume the legitimate visit recorded before replacement; the stale
        // provider connection must not keep granting an exemption afterward.
        check(h.remainingFor(provider) == 10, "valid prior provider visit disappeared");
        h.exhaustFor(provider);
        h.release(h.process, fresh, true);
        h.flush();
    }

    private static void concurrency(boolean parcel) throws Exception {
        Harness h = new Harness(parcel, null, false);
        var executor = Executors.newFixedThreadPool(8);
        CountDownLatch start = new CountDownLatch(1);
        AtomicInteger accepted = new AtomicInteger();
        List<java.util.concurrent.Future<?>> futures = new ArrayList<>();
        try {
            for (int i = 0; i < 8; i++) futures.add(executor.submit(() -> {
                try {
                    if (!start.await(5, TimeUnit.SECONDS)) throw new AssertionError("start barrier timed out");
                    h.app(() -> { for (int j = 0; j < 32; j++) if (h.api.setDynamicShortcuts(OWNER,
                            ParceledListSlice.emptyList(), 0)) accepted.incrementAndGet(); });
                } catch (Exception failure) { throw new AssertionError(failure); }
            }));
            start.countDown();
            for (var future : futures) future.get(15, TimeUnit.SECONDS);
            check(accepted.get() == 10 && h.remaining(OWNER) == 0, "concurrent callers exceeded one publication budget");
        } finally { executor.shutdownNow(); check(executor.awaitTermination(5, TimeUnit.SECONDS), "quota workers leaked"); }
        h.flush();
    }

    private static void clockEdges(boolean parcel) throws Exception {
        Harness h = new Harness(parcel, null, false);
        h.exhaust(OWNER);
        h.clock.set(1_000_000);
        check(h.remaining(OWNER) == 0, "invalid early-boot clock reset a persisted budget");
        h.clock.set(NOW + 2 * DAY + 1);
        check(h.remaining(OWNER) == 10, "multi-day rollover missing");
        long aligned = h.clock.get() / DAY * DAY + NOW % DAY;
        h.app(() -> check(h.api.getRateLimitResetTime(OWNER, 0) == aligned + DAY,
                "daily reset lost the original wall-clock phase"));
        h.clock.set(Long.MAX_VALUE);
        h.app(() -> check(h.api.getRateLimitResetTime(OWNER, 0) == Long.MAX_VALUE,
                "far-future reset overflowed to a negative deadline"));
        h.flush();
        Harness phase = new Harness(parcel, null, false);
        long day0 = NOW / DAY * DAY;
        phase.clock.set(day0 + DAY / 2); phase.exhaust(OWNER);
        phase.clock.set(day0 + 2 * DAY + 60_000);
        phase.app(() -> {
            check(phase.api.getRateLimitResetTime(OWNER, 0) == day0 + 3 * DAY + DAY / 2,
                    "pinned phase-crossing first access changed");
            check(phase.api.getRateLimitResetTime(OWNER, 0) == day0 + 3 * DAY + 60_000,
                    "pinned phase-crossing second access did not take clock rewind");
        });
        check(phase.remaining(OWNER) == 10, "phase-crossing reset did not reach the publisher");
        phase.flush();
    }

    @SuppressWarnings("unchecked")
    private static void retiredActivity(boolean parcel) throws Exception {
        Harness h = new Harness(parcel, null, false);
        h.exhaust(OWNER);
        Object activity = h.activity();
        List<Object> activities = (List<Object>) get(h.atm, "mActivities");
        activities.remove(activity);
        // Model retirement after ActivityClientController authenticated its token,
        // before the separately locked resume acknowledgment reaches the controller.
        call(h.atm, "activityResumed", new Class<?>[]{activity.getClass()}, activity);
        set(activity, "state", state("STOPPED"));
        check(h.remaining(OWNER) == 0, "retired activity acknowledgment restored quota");
        activities.add(activity); set(activity, "state", state("RESUMING"));
        List<Object> processes = (List<Object>) get(h.atm, "mProcesses");
        processes.remove(h.process);
        call(h.atm, "activityResumed", new Class<?>[]{activity.getClass()}, activity);
        set(activity, "state", state("STOPPED")); processes.add(h.process);
        check(h.remaining(OWNER) == 0, "detached process acknowledgment restored quota");
        activities.remove(activity); ((List<Object>) get(h.process, "activities")).clear();
        h.flush();
    }

    private static void writeFailure(boolean parcel, Path file) throws Exception {
        Files.createDirectory(file);
        Harness h = new Harness(parcel, file, false);
        h.exhaust(OWNER);
        check(!(Boolean) call(h.implementation, "flushQuota", new Class<?>[0]), "directory accepted as quota file");
        check(h.remaining(OWNER) == 0, "failed persistence restored a consumed quota");
        Files.delete(file); h.flush(); h.reopen(file, false);
        check(h.remaining(OWNER) == 0, "dirty quota was not retried at explicit durability boundary"); h.flush();
    }

    private static void earlyClockPersistence(boolean parcel, Path file) throws Exception {
        Harness h = new Harness(parcel, file, false);
        h.clock.set(-1000); h.exhaust(OWNER); h.flush(); h.reopen(file, false);
        check(h.remaining(OWNER) == 0, "invalid early-boot clock lost durable quota on restart");
        h.clock.set(NOW);
        check(h.remaining(OWNER) == 10, "valid clock did not advance the early-boot anchor");
        h.app(() -> check(h.api.getRateLimitResetTime(OWNER, 0) == NOW / DAY * DAY - 1000 + DAY,
                "negative initial offset overflowed the daily phase")); h.flush();
    }

    @SuppressWarnings("unchecked")
    private static void installation(boolean parcel, Path file) throws Exception {
        Harness h = new Harness(parcel, file, false);
        h.exhaust(OWNER); h.flush();
        call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner);
        h.app(() -> rejects(SecurityException.class, () -> h.api.getRemainingCallCount(OWNER, 0), "pending removal"));
        call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, OWNER);
        call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner);
        check(h.remaining(OWNER) == 0, "aborted uninstall erased quota");
        ApkPackage update = code(OWNER, h.owner.firstInstallTime());
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, update);
        set(h.process, "packageCode", update);
        check(h.remaining(OWNER) == 0, "same-installation update reset quota");
        h.flush(); h.reopen(file, false);
        check(h.remaining(OWNER) == 0, "updated owner lost persisted quota");
        call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, update);
        call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, OWNER);
        call(h.pm, "remove", new Class<?>[]{String.class}, OWNER);
        call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, update);
        call(h.pm, "packageRemoved", new Class<?>[]{ApkPackage.class, Object.class}, update, h.pm);
        ((java.util.concurrent.ExecutorService) get(h.pm, "mPackageEventExecutor")).submit(() -> { }).get(5, TimeUnit.SECONDS);
        Object quota = get(h.implementation, "mQuota");
        check(!((Map<?, ?>) get(quota, "mCounters")).containsKey(OWNER), "committed removal retained quota ownership");
        ApkPackage reinstall = code(OWNER, update.firstInstallTime() + 1);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, reinstall);
        set(h.process, "packageCode", reinstall);
        check(h.remaining(OWNER) == 10, "reinstall inherited its UID tombstone's old budget");
        h.exhaust(OWNER);
        // Committed events can run after another installation. A delayed old event
        // must not erase counters already owned by the current installed generation.
        call(h.pm, "packageRemoved", new Class<?>[]{ApkPackage.class, Object.class}, update, h.pm);
        ((java.util.concurrent.ExecutorService) get(h.pm, "mPackageEventExecutor")).submit(() -> { }).get(5, TimeUnit.SECONDS);
        check(h.remaining(OWNER) == 0, "late removed event erased the new installation's budget");
        h.flush();
        // Restore filters owner identity before publishing any counters.
        ApkPackage next = code(OWNER, reinstall.firstInstallTime() + 1);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, next); set(h.process, "packageCode", next);
        h.reopen(file, false);
        check(h.remaining(OWNER) == 10, "restore adopted a previous first-install lifetime");
        h.flush();
    }

    private static void permissions(boolean parcel) throws Exception {
        Harness h = new Harness(parcel, null, false);
        ApkPackage publisher = code(OWNER, h.owner.firstInstallTime(), Manifest.permission.UNLIMITED_SHORTCUTS_API_CALLS,
                Manifest.permission.RESET_SHORTCUT_MANAGER_THROTTLING);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, publisher); set(h.process, "packageCode", publisher);
        ApkPackage platform = code("android", 100);
        set(platform, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{8, 9})});
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, platform);
        h.exhaust(OWNER);
        h.app(() -> {
            check(!h.api.setDynamicShortcuts(OWNER, ParceledListSlice.emptyList(), 0), "requested signature permission granted without signer");
            rejects(SecurityException.class, () -> h.api.onApplicationActive(OWNER, 0), "unsigned privileged reset");
        });
        ApkPackage matchingPlatform = code("android", 100);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, matchingPlatform);
        h.app(() -> {
            for (int i = 0; i < 24; i++) check(h.api.setDynamicShortcuts(OWNER,
                    ParceledListSlice.emptyList(), 0), "signature-authorized unlimited API throttled");
            check(h.api.getRemainingCallCount(OWNER, 0) == 10, "unlimited permission query retained debt");
            h.api.onApplicationActive(OWNER, 0);
            rejects(SecurityException.class, h.api::resetThrottling, "signature holder obtained system/shell reset");
        });
        // Removing the installed permission owner immediately removes the exemption.
        call(h.pm, "remove", new Class<?>[]{String.class}, "android");
        h.exhaust(OWNER);
        for (int uid : new int[]{android.os.Process.SYSTEM_UID, android.os.Process.SHELL_UID, android.os.Process.ROOT_UID}) {
            long identity = Binder.clearCallingIdentity();
            try { Binder.restoreCallingIdentity(((long) uid << 32) | (PID + 90)); h.api.resetThrottling(); }
            finally { Binder.restoreCallingIdentity(identity); }
            check(h.remaining(OWNER) == 10, "authorized system/shell/root reset failed: " + uid);
            h.exhaust(OWNER);
        }
        h.flush();
    }

    private static void corruption(boolean parcel, Path file) throws Exception {
        Harness h = new Harness(parcel, file, false);
        h.exhaust(OWNER); h.exhaust(PEER); h.flush();
        byte[] valid = Files.readAllBytes(file);
        for (int length : new int[]{0, 4, 12, valid.length / 2, valid.length - 1}) {
            Files.write(file, Arrays.copyOf(valid, length)); h.reopen(file, false);
            check(h.remaining(OWNER) == 10 && h.remaining(PEER) == 10, "truncation published partial quota state: " + length);
            h.flush();
        }
        byte[] trailing = Arrays.copyOf(valid, valid.length + 1);
        Files.write(file, trailing); h.reopen(file, false);
        check(h.remaining(OWNER) == 10 && h.remaining(PEER) == 10, "trailing data was trusted"); h.flush();
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (DataOutputStream output = new DataOutputStream(bytes)) {
            output.writeInt(0x47535131); output.writeLong(NOW); output.writeUTF("en-US"); output.writeInt(Integer.MAX_VALUE);
        }
        Files.write(file, bytes.toByteArray()); h.reopen(file, false);
        check(h.remaining(OWNER) == 10, "unbounded decoded record count admitted"); h.flush();
        Files.write(file, valid); h.reopen(file, false);
        check(h.remaining(OWNER) == 0 && h.remaining(PEER) == 0, "valid complete state did not recover");
        h.flush();
    }

    public static void workload(int expected) throws Exception {
        Harness h = new Harness(false, null, true);
        int[] published = {0};
        long started = System.nanoTime();
        h.app(() -> {
            for (int i = 0; i < 10_000; i++) if (h.api.setDynamicShortcuts(OWNER, ParceledListSlice.emptyList(), 0)) published[0]++;
        });
        try { h.flush(); }
        catch (NoSuchMethodException baseline) { if (expected != 10_000) throw baseline; }
        long elapsed = System.nanoTime() - started;
        System.out.println("SHORTCUT_QUOTA_WORK {\"attempts\":10000,\"published\":" + published[0]
                + ",\"rejected\":" + (10_000 - published[0]) + ",\"elapsed_ns\":" + elapsed + "}");
        check(published[0] == expected, "background publication count: " + published[0]);
        System.out.println("Actual DesktopShortcutService and AppShell empty-snapshot disk publication, including quota durability flush; constructed package/process metadata, shell presentation disabled.");
    }

    public static void contracts(boolean parcel) throws Exception {
        checks = 0;
        isolateCacheTransport();
        Path directory = Files.createTempDirectory("gesher-shortcut-quota-");
        Path file = directory.resolve("quota.bin");
        try {
            basics(parcel, file); validation(parcel); foreground(parcel); dependencies(parcel); providers(parcel);
            evaluationFailure(parcel); powerVisits(parcel);
            concurrency(parcel); clockEdges(parcel);
            retiredActivity(parcel);
            installation(parcel, directory.resolve("installation.bin")); permissions(parcel);
            corruption(parcel, directory.resolve("corruption.bin"));
            writeFailure(parcel, directory.resolve("failed.bin"));
            earlyClockPersistence(parcel, directory.resolve("early-clock.bin"));
        } finally {
            try (var paths = Files.walk(directory)) {
                for (Path path : paths.sorted(java.util.Comparator.reverseOrder()).toList()) Files.delete(path);
            }
        }
        System.out.println("ShortcutQuotaContractTest passed: " + checks + "; real_parcel=" + parcel);
    }

    private static void isolateCacheTransport() throws Exception {
        if (cacheTransportIsolated) return;
        // This isolated controller has no system-server shared-memory transport.
        android.app.PropertyInvalidatedCache.setTestMode(true);
        ((android.app.PropertyInvalidatedCache<?, ?>) field(android.content.pm.PackageManager.class,
                "sPackageInfoCache").get(null)).testPropertyName();
        cacheTransportIsolated = true;
    }

    private static void measured(String stage, int nodes, int operations, AtomicLong edgeChecks,
            LongSupplier cpu, LongSupplier allocated, Action action) throws Exception {
        edgeChecks.set(0);
        long beforeCpu = cpu.getAsLong(), beforeHeap = allocated.getAsLong(), before = System.nanoTime();
        action.run();
        long elapsed = System.nanoTime() - before, heap = allocated.getAsLong() - beforeHeap;
        long cpuTime = cpu.getAsLong() - beforeCpu;
        System.out.println("SHORTCUT_GRAPH_WORK {\"stage\":\"" + stage + "\",\"processes\":" + (nodes + 1)
                + ",\"connections\":" + (nodes + 1) + ",\"operations\":" + operations
                + ",\"lifetime_checks\":" + edgeChecks.get() + ",\"allocated_bytes\":" + heap
                + ",\"thread_cpu_ns\":" + cpuTime + ",\"elapsed_ns\":" + elapsed + "}");
    }

    /** Host resource workload; no timing/HotSpot assumption is used by the ART contracts. */
    @SuppressWarnings("unchecked")
    public static void graphWorkload(int nodes, int queries, LongSupplier cpu, LongSupplier allocated) throws Exception {
        if (nodes < 2 || queries < 1) throw new IllegalArgumentException("graph workload dimensions");
        isolateCacheTransport();
        Harness h = new Harness(false, null, false);
        set(h.process, "inProcessActivityState", state("STOPPED"));
        List<Object> processes = new ArrayList<>();
        List<IServiceConnection> connections = new ArrayList<>();
        for (int i = 0; i < nodes; i++) {
            Object process = h.extra("fixture.shortcut.graph.p" + i, UID + 100 + i, PID + 100 + i);
            h.service(process); processes.add(process);
        }
        Object client = h.process;
        for (Object target : processes) {
            connections.add(h.bind(client, target, 0)); client = target;
        }
        connections.add(h.bind(client, processes.get(0), 0)); // a cycle without an independent root
        Object target = processes.get(nodes - 1);
        h.exhaustFor(target);
        Object services = get(h.am, "mServices");
        AtomicLong edgeChecks = new AtomicLong();
        for (List<Object> list : ((Map<IBinder, List<Object>>) get(services, "mConnections")).values()) {
            for (Object connection : list) {
                java.util.function.BooleanSupplier original = (java.util.function.BooleanSupplier) get(connection, "lifetime");
                set(connection, "lifetime", (java.util.function.BooleanSupplier) () -> {
                    edgeChecks.incrementAndGet(); return original.getAsBoolean();
                });
            }
        }
        synchronized (services) { call(services, "publishShortcutStateLocked", new Class<?>[0]); }
        Object snapshot = get(services, "mShortcutSnapshot");
        String name = (String) get(target, "packageName");
        for (int i = 0; i < 100; i++) check(h.remainingFor(target) == 0, "background graph warmup quota");
        measured("background_denied_publications", nodes, queries, edgeChecks, cpu, allocated, () -> as(target, () -> {
            for (int i = 0; i < queries; i++) check(!h.api.setDynamicShortcuts(name, ParceledListSlice.emptyList(), 0),
                    "background dependency graph manufactured allowance");
        }));
        check(edgeChecks.get() == 0, "background graph traversed dependencies without a foreground root");
        check(get(services, "mShortcutSnapshot") == snapshot, "query rebuilt service topology");
        h.manualState("RESUMED");
        for (int i = 0; i < 100; i++) check(h.remainingFor(target) == 10, "foreground graph warmup quota");
        measured("foreground_allowance_queries", nodes, queries, edgeChecks, cpu, allocated, () -> as(target, () -> {
            for (int i = 0; i < queries; i++) check(h.api.getRemainingCallCount(name, 0) == 10,
                    "actual reachable dependency was throttled");
        }));
        check(edgeChecks.get() == (long) queries * (nodes + 1), "query repeated or skipped real dependency work");
        check(get(services, "mShortcutSnapshot") == snapshot, "foreground query rebuilt topology");
        h.manualState("STOPPED");
        measured("foreground_visits", nodes, 32, edgeChecks, cpu, allocated, () -> {
            for (int i = 0; i < 32; i++) { h.manualState("RESUMED"); h.manualState("STOPPED"); }
        });
        check(edgeChecks.get() == 32L * (nodes + 1), "visit propagation repeated a full fixed-point scan");
        h.manualState("RESUMED");
        measured("root_connection_retirement", nodes, 1, edgeChecks, cpu, allocated,
                () -> h.unbind(h.process, connections.get(0)));
        check(get(services, "mShortcutSnapshot") != snapshot, "removed connection retained old topology");
        h.exhaustFor(target);
        measured("retired_root_denied_publications", nodes, queries, edgeChecks, cpu, allocated, () -> as(target, () -> {
            for (int i = 0; i < queries; i++) check(!h.api.setDynamicShortcuts(name, ParceledListSlice.emptyList(), 0),
                    "cycle retained its removed foreground dependency");
        }));
        check(edgeChecks.get() == 0, "retired graph walked unreachable connections");
        measured("root_connection_publication", nodes, 1, edgeChecks, cpu, allocated,
                () -> h.bind(h.process, processes.get(0), 0));
        check(h.remainingFor(target) == 10, "new owned root did not restore allowance");
        evaluationCleared(h.am);
        Object scratch = get(h.am, "mShortcutEvaluation");
        Object priorMembers = get(scratch, "members"), priorStates = get(scratch, "states");
        // Controlled registry retirement checks capacity ownership; it is not
        // included in the measured real bind/unbind transition receipts above.
        synchronized (h.atm) {
            ((List<Object>) get(h.atm, "mProcesses")).removeAll(processes);
            call(h.am, "refreshShortcutForeground", new Class<?>[0]);
        }
        evaluationCleared(h.am);
        if (nodes >= 64) check(get(scratch, "members") != priorMembers && get(scratch, "states") != priorStates,
                "retired process graph retained peak work-table capacity");
        System.out.println("Actual current-process/ActiveServices binding snapshots and DesktopShortcutService calls; lifetime observers count real predicates, never replace their result. Host CPU/allocation only; no ART timing claim.");
    }
}
