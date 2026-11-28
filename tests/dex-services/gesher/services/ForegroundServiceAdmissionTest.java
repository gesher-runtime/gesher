/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.IBinderSession;
import android.app.IServiceConnection;
import android.app.InvalidForegroundServiceTypeException;
import android.app.MissingForegroundServiceTypeException;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.RemoteServiceException.ForegroundServiceDidNotStartInTimeException;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ParceledListSlice;
import android.content.pm.PermissionInfo;
import android.content.pm.ServiceInfo;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import android.service.notification.StatusBarNotification;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.NotificationManagerService;
import com.gesher.server.PackageManagerService;
import com.gesher.server.StandIn;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.ToIntBiFunction;
import sun.misc.Unsafe;

/** Actual AM/ActiveServices promotion and PermissionPolicy; synthetic installed metadata. */
public final class ForegroundServiceAdmissionTest {
    private static final String OWNER = "fixture.foreground.owner";
    private static final int UID = 12772, PID = 884772;
    private static final ComponentName SERVICE = new ComponentName(OWNER, OWNER + ".ForegroundService");
    private static final String BASE = Manifest.permission.FOREGROUND_SERVICE;
    private static final String MEDIA = Manifest.permission.FOREGROUND_SERVICE_MEDIA_PLAYBACK;
    private static final String SPECIAL = Manifest.permission.FOREGROUND_SERVICE_SPECIAL_USE;
    private static final Unsafe UNSAFE;
    private static boolean binder;
    private static int checks;
    private static FixtureServiceContext.Scope contexts;
    static {
        try {
            Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true);
            UNSAFE = (Unsafe) f.get(null);
        } catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Work { void run() throws Exception; }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Field field(Class<?> cls, String name) throws Exception {
        Field f = cls.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Object get(Object obj, String name) throws Exception { return field(obj.getClass(), name).get(obj); }
    private static void set(Object obj, String name, Object value) throws Exception { field(obj.getClass(), name).set(obj, value); }
    private static Object make(Class<?> cls, Class<?>[] types, Object... args) throws Exception {
        Constructor<?> c = cls.getDeclaredConstructor(types); c.setAccessible(true);
        try { return c.newInstance(args); }
        catch (InvocationTargetException error) { throw new AssertionError("Fixture construction failed", error.getCause()); }
    }
    private static Object call(Object obj, String name, Class<?>[] types, Object... args) throws Exception {
        var method = obj.getClass().getDeclaredMethod(name, types); method.setAccessible(true);
        try { return method.invoke(obj, args); }
        catch (InvocationTargetException error) {
            if (error.getCause() instanceof Exception cause) throw cause;
            throw new AssertionError("Fixture publication failed", error.getCause());
        }
    }
    private static void rejects(Class<? extends Throwable> type, Work work, String message) throws Exception {
        try { work.run(); }
        catch (Throwable failure) {
            if (type.isInstance(failure)) { checks++; return; }
            throw new AssertionError(message + ": wrong exception " + failure, failure);
        }
        throw new AssertionError(message + ": operation was accepted");
    }
    private static IBinder remote(IBinder target) {
        return new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException { return target.transact(code, data, reply, flags); }
        };
    }
    private static ApkPackage code(int targetSdk, int manifestType, String... permissions) throws Exception {
        return code(OWNER, UID, targetSdk, manifestType, permissions);
    }
    private static ApkPackage code(String name, int uid, int targetSdk, int manifestType,
            String... permissions) throws Exception {
        String path = "/fixture/foreground/" + name + "/base.apk";
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, path);
        parsed.setEnabled(true); parsed.setTargetSdkVersion(targetSdk);
        parsed.setSharedUserId("fixture.foreground.shared." + uid);
        for (String permission : permissions) {
            parsed.addUsesPermission(new ParsedUsesPermissionImpl(permission, 0, java.util.Set.of()));
        }
        parsed.addUsesPermission(new ParsedUsesPermissionImpl(Manifest.permission.POST_NOTIFICATIONS,
                0, java.util.Set.of()));
        ParsedServiceImpl service = new ParsedServiceImpl();
        service.setPackageName(name); service.setName(name + ".ForegroundService");
        service.setEnabled(true); service.setExported(false); service.setProcessName(name);
        service.setForegroundServiceType(manifestType);
        parsed.addService(service); parsed.hideAsFinal();
        ApkPackage code = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        set(code, "packageName", name); set(code, "parsed", parsed); set(code, "mUid", uid);
        set(code, "apkPath", path); set(code, "mDataDir", "/fixture/foreground/" + name + "/data");
        set(code, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        set(code, "mSigningIdentityChecked", true);
        set(code, "mVerifiedSignatures", new android.content.pm.Signature[]{
                new android.content.pm.Signature(new byte[]{7, 2, 1})});
        return code;
    }
    private static final class AppThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        IBinder service;
        int creates;
        int crashes;
        String crashMessage;
        int crashType;
        Bundle crashExtras;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleCreateService(IBinder token, ServiceInfo info,
                CompatibilityInfo compatibility, int processState) { service = token; creates++; }
        @Override public void scheduleCrash(String message, int type, Bundle extras) {
            crashes++;
            crashMessage = message;
            crashType = type;
            crashExtras = extras;
        }
    }
    private static final class Harness {
        final ApkPackage owner;
        final PackageManagerService pm;
        final Object atm, services, process;
        final IActivityManager am;
        final NotificationManagerService notifications;
        final AppThread thread = new AppThread();
        final AtomicInteger posts = new AtomicInteger();
        Harness(int target, int manifestType, String... permissions) throws Exception {
            this(target, manifestType, List.of(), permissions);
        }
        @SuppressWarnings("unchecked")
        Harness(int target, int manifestType, List<ApkPackage> siblings, String... permissions) throws Exception {
            owner = code(target, manifestType, permissions);
            List<ApkPackage> installed = new ArrayList<>(siblings); installed.add(owner);
            pm = (PackageManagerService) make(PackageManagerService.class,
                    new Class<?>[]{Collection.class, boolean.class}, installed, false);
            set(pm, "mPackageEventSender", (java.util.function.Consumer<Intent>) event -> {});
            if (!binder) {
                // Only the host AssetManager boundary: actual Android normal-permission
                // definitions. The permission checks and installed declarations stay real.
                Object catalog = get(pm.permissions(), "mCatalog");
                Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>)get(catalog, "mFramework");
                List<String> names = new ArrayList<>(List.of(TYPE_PERMISSIONS)); names.add(BASE);
                for (String name : names) {
                    PermissionInfo info = new PermissionInfo(); info.name = name; info.packageName = "android";
                    info.protectionLevel = PermissionInfo.PROTECTION_NORMAL; definitions.put(name, info);
                }
                PermissionInfo post = new PermissionInfo(); post.name = Manifest.permission.POST_NOTIFICATIONS;
                post.packageName = "android"; post.protectionLevel = PermissionInfo.PROTECTION_DANGEROUS;
                definitions.put(post.name, post);
                set(catalog, "mLoaded", true);
            }
            atm = make(type("ActivityTaskManagerService"), new Class<?>[]{PackageManagerService.class}, pm);
            ToIntBiFunction<StatusBarNotification, Boolean> sink = (notice, update) -> { posts.incrementAndGet(); return 0; };
            notifications = (NotificationManagerService) make(NotificationManagerService.class,
                    new Class<?>[]{PackageManagerService.class, type("ActivityManagerService"), ToIntBiFunction.class},
                    pm, null, sink);
            Object implementation = make(type("ActivityManagerService"),
                    new Class<?>[]{atm.getClass(), PackageManagerService.class, NotificationManagerService.class},
                    atm, pm, notifications);
            set(notifications, "mActivityManager", implementation);
            set(notifications, "mListenersBound", true);
            services = get(implementation, "mServices");
            field(type("Services"), "sLast").set(null, contexts.runtime(pm, atm, implementation));
            pm.permissions().grantRuntimePermission(OWNER, Manifest.permission.POST_NOTIFICATIONS, "default:0", 0);
            long original = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) UID << 32) | PID);
                process = make(type("ActivityTaskManagerService$ProcessRecord"),
                        new Class<?>[]{atm.getClass(), int.class, IApplicationThread.class}, atm, PID, thread);
            } finally { Binder.restoreCallingIdentity(original); }
            set(process, "packageName", OWNER); set(process, "processName", OWNER);
            set(process, "packageCode", owner); set(process, "ready", true);
            ((List<Object>)get(atm, "mProcesses")).add(process);
            am = binder ? IActivityManager.Stub.asInterface(remote(
                    StandIn.of(IActivityManager.class, (IActivityManager) implementation).asBinder()))
                    : (IActivityManager) implementation;
            if (target >= 26) {
                notifications.createNotificationChannels(OWNER, new ParceledListSlice<>(List.of(
                        new NotificationChannel("fgs", "Foreground contract", 3))));
            }
        }
        void asApp(Work action) throws Exception { asUid(UID, PID, action); }
        void asUid(int uid, int pid, Work action) throws Exception {
            long original = Binder.clearCallingIdentity();
            try { Binder.restoreCallingIdentity(((long)uid << 32) | pid); action.run(); }
            finally { Binder.restoreCallingIdentity(original); }
        }
        void start(boolean foreground) throws Exception {
            asApp(() -> check(SERVICE.equals(am.startService(thread, new Intent().setComponent(SERVICE),
                    null, foreground, OWNER, null, 0)), "Service launch must retain the production resolution path"));
            check(thread.service != null && thread.creates == 1, "Service must be created exactly once");
        }
        void stop() throws Exception {
            asApp(() -> check(am.stopService(thread, new Intent().setComponent(SERVICE), null, 0) == 1,
                    "The service must have a started lifetime to stop"));
        }
        Object promotionDeadline() throws Exception { return get(thread.service, "foregroundStartDeadline"); }
        void expire(Object deadline) throws Exception {
            call(services, "foregroundStartTimedOut",
                    new Class<?>[]{thread.service.getClass(), deadline.getClass()}, thread.service, deadline);
        }
        Notification notice() throws Exception {
            Notification n = new Notification(); field(Notification.class, "mChannelId").set(n, "fgs");
            n.setSmallIcon(android.graphics.drawable.Icon.createWithResource("android", android.R.drawable.ic_dialog_info));
            n.extras = new android.os.Bundle(); n.extras.putString(Notification.EXTRA_TITLE, "Foreground admission fixture");
            return n;
        }
        void promote(int id, int type) throws Exception {
            Notification n = notice();
            asApp(() -> am.setServiceForeground(SERVICE, thread.service, id, n, 0, type));
        }
        int selected() throws Exception {
            int[] selected = {-1}; asApp(() -> selected[0] = am.getForegroundServiceType(SERVICE, thread.service));
            return selected[0];
        }
        void unchanged(int id, int type, int capabilities, int published) throws Exception {
            check((Integer)get(thread.service, "foregroundId") == id && selected() == type,
                    "Rejected promotion changed the foreground ID/type");
            check((Integer)get(thread.service, "foregroundCapabilities") == capabilities,
                    "Rejected promotion changed while-in-use capabilities");
            check(posts.get() == published, "Rejected promotion published a notification");
        }
    }
    private static final int[] TYPES = {
        ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK,
        ServiceInfo.FOREGROUND_SERVICE_TYPE_PHONE_CALL, ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION,
        ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION,
        ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA, ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE,
        ServiceInfo.FOREGROUND_SERVICE_TYPE_HEALTH, ServiceInfo.FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING,
        ServiceInfo.FOREGROUND_SERVICE_TYPE_SYSTEM_EXEMPTED, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROCESSING,
        ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
    };
    private static final String[] TYPE_PERMISSIONS = {
        Manifest.permission.FOREGROUND_SERVICE_DATA_SYNC, MEDIA,
        Manifest.permission.FOREGROUND_SERVICE_PHONE_CALL, Manifest.permission.FOREGROUND_SERVICE_LOCATION,
        Manifest.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE, Manifest.permission.FOREGROUND_SERVICE_MEDIA_PROJECTION,
        Manifest.permission.FOREGROUND_SERVICE_CAMERA, Manifest.permission.FOREGROUND_SERVICE_MICROPHONE,
        Manifest.permission.FOREGROUND_SERVICE_HEALTH, Manifest.permission.FOREGROUND_SERVICE_REMOTE_MESSAGING,
        Manifest.permission.FOREGROUND_SERVICE_SYSTEM_EXEMPTED, Manifest.permission.FOREGROUND_SERVICE_MEDIA_PROCESSING,
        SPECIAL
    };
    private static void basePermission() throws Exception {
        Harness legacy = new Harness(27, 0); legacy.start(true); legacy.promote(1, 0);
        check(legacy.posts.get() == 1, "Target 27 promotion must not require the P permission");
        Harness modern = new Harness(28, 0); modern.start(true);
        rejects(SecurityException.class, () -> modern.promote(1, 0), "Target 28 missing FOREGROUND_SERVICE");
        modern.unchanged(0, 0, 0, 0);
        // A system caller cannot lend its permission to the service's ordinary app UID.
        rejects(SecurityException.class, () -> modern.am.setServiceForeground(SERVICE, modern.thread.service,
                1, modern.notice(), 0, 0), "System caller cannot authorize an unprivileged service owner");
        call(modern.services, "stopPackage", new Class<?>[]{String.class}, OWNER);
        Harness permitted = new Harness(28, 0, BASE); permitted.start(false); permitted.promote(2, 0);
        check(permitted.posts.get() == 1, "An ordinary started service may promote with its manifest permission");
    }
    private static void typeBoundaries() throws Exception {
        Harness legacy = new Harness(33, 0, BASE); legacy.start(false);
        legacy.promote(1, ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST);
        check(legacy.selected() == 0, "Target 33 missing manifest types remain compatible");
        Harness missing = new Harness(34, 0, BASE); missing.start(false);
        rejects(MissingForegroundServiceTypeException.class,
                () -> missing.promote(1, ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST), "Manifest sentinel without types");
        rejects(InvalidForegroundServiceTypeException.class, () -> missing.promote(1, 0), "Explicit NONE type");
        missing.unchanged(0, 0, 0, 0);
        Harness declared = new Harness(34, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK, BASE, MEDIA);
        declared.start(false);
        rejects(InvalidForegroundServiceTypeException.class, () -> declared.promote(1, 0), "Explicit NONE ignores declared types");
        rejects(IllegalArgumentException.class,
                () -> declared.promote(1, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC), "Undeclared type rejected");
        declared.promote(2, ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST);
        check(declared.selected() == ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK && declared.posts.get() == 1,
                "Manifest sentinel selects the real declaration and publishes its notification");
        Harness oldType = new Harness(33, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK, BASE);
        oldType.start(false); oldType.promote(1, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        check(oldType.posts.get() == 1, "Type-specific permissions are not enforced below target 34");
        rejects(IllegalArgumentException.class,
                () -> oldType.promote(2, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC), "Legacy manifest subset still enforced");
    }
    private static void manifestPermissions() throws Exception {
        // Negative normal-permission checks cover every enabled default type. This is
        // not an assertion that its separate runtime/device prerequisites are implemented.
        for (int i = 0; i < TYPES.length; i++) {
            Harness missing = new Harness(34, TYPES[i], BASE); missing.start(false);
            final int type = TYPES[i];
            rejects(SecurityException.class, () -> missing.promote(1, type), "Missing " + TYPE_PERMISSIONS[i]);
            missing.unchanged(0, 0, 0, 0);
        }
        int union = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK | ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC;
        Harness h = new Harness(36, union, BASE, MEDIA); h.start(false);
        h.promote(3, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        rejects(SecurityException.class, () -> h.promote(4, union), "Every selected type needs its own normal permission");
        h.unchanged(3, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK, 0, 1);
        Harness both = new Harness(36, union, BASE, MEDIA, Manifest.permission.FOREGROUND_SERVICE_DATA_SYNC);
        both.start(false); both.promote(5, union);
        check(both.selected() == union && both.posts.get() == 1, "Declared permitted multi-type promotion failed");
    }
    private static void normalizationAndStop() throws Exception {
        int shortType = ServiceInfo.FOREGROUND_SERVICE_TYPE_SHORT_SERVICE;
        Harness shortService = new Harness(34, shortType, BASE); shortService.start(false);
        shortService.promote(1, shortType);
        check(shortService.selected() == shortType, "SHORT_SERVICE needs no additional normal permission");
        int combined = shortType | ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
        Harness combinedService = new Harness(34, combined, BASE, MEDIA); combinedService.start(false);
        combinedService.promote(1, combined);
        check(combinedService.selected() == ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK,
                "SHORT_SERVICE must be ignored when combined with other types");
        for (int unknown : new int[]{1 << 20, ServiceInfo.FOREGROUND_SERVICE_TYPE_FILE_MANAGEMENT}) {
            Harness h = new Harness(34, unknown, BASE); h.start(false);
            rejects(InvalidForegroundServiceTypeException.class, () -> h.promote(1, unknown), "Unregistered default type");
            Harness fallback = new Harness(34, unknown | ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE, BASE, SPECIAL);
            fallback.start(false); fallback.promote(1, ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST);
            check(fallback.selected() == (unknown | ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE),
                    "SPECIAL_USE preserves a declared unknown future type under AOSP's fallback policy");
        }
        Harness stopped = new Harness(36, 0); stopped.start(false);
        set(stopped.thread.service, "foregroundId", 17); set(stopped.thread.service, "whileInUseAtStart", true);
        stopped.asApp(() -> stopped.am.setServiceForeground(SERVICE, stopped.thread.service, 0, null,
                Service.STOP_FOREGROUND_DETACH, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA));
        stopped.unchanged(0, 0, 0, 0);
        check(!(Boolean)get(stopped.thread.service, "whileInUseAtStart"), "stopForeground must clear prior admission");
        // The same checks must still be protected by the service token's owner.
        rejects(SecurityException.class, () -> combinedService.asUid(UID + 1, PID + 1,
                () -> combinedService.am.setServiceForeground(SERVICE, combinedService.thread.service,
                        2, combinedService.notice(), 0, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)),
                "A foreign caller may not use another application's service token");
        combinedService.unchanged(1, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK, 0, 1);
    }
    private static void sharedUidTargets() throws Exception {
        String sibling = "fixture.foreground.sibling";
        int media = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
        ApkPackage legacy = code(sibling, UID, 33, 0, BASE);
        Harness shared = new Harness(34, media, List.of(legacy));
        check(shared.pm.getPackagesForUid(UID).length == 2,
                "Shared-UID fixture must expose both installed packages through PackageManager");
        shared.start(false);
        shared.promote(1, 0);
        shared.promote(2, media);
        check(shared.selected() == media && shared.posts.get() == 2,
                "A target-33 shared sibling keeps NONE and type permissions permissive for a target-34 service");
        // Use actual PM replacement publication rather than modifying admitted metadata.
        call(shared.pm, "add", new Class<?>[]{ApkPackage.class}, code(sibling, UID, 34, 0, BASE));
        rejects(InvalidForegroundServiceTypeException.class, () -> shared.promote(3, 0),
                "All UID members reaching target 34 must enable NONE rejection");
        rejects(SecurityException.class, () -> shared.promote(3, media),
                "All UID members reaching target 34 must enable normal type permission checks");
        shared.unchanged(2, media, 0, 2);
        call(shared.pm, "add", new Class<?>[]{ApkPackage.class}, code(sibling, UID, 34, 0, BASE, MEDIA));
        shared.promote(3, media);
        check(shared.selected() == media && shared.posts.get() == 3,
                "The service receives real normal permissions granted to its shared UID sibling");
        call(shared.pm, "remove", new Class<?>[]{String.class}, sibling);
        rejects(SecurityException.class, () -> shared.promote(4, media),
                "Removing the only permission-requesting sibling must remove its UID grant");
        shared.unchanged(3, media, 0, 3);

        Harness removal = new Harness(34, 0, List.of(code(sibling, UID, 33, 0)), BASE);
        removal.start(false); removal.promote(1, ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST);
        call(removal.pm, "remove", new Class<?>[]{String.class}, sibling);
        rejects(MissingForegroundServiceTypeException.class,
                () -> removal.promote(2, ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST),
                "Removing a legacy sibling must recompute the current UID compatibility gate");
        removal.unchanged(1, 0, 0, 1);

        Harness unrelated = new Harness(34, 0, List.of(code(sibling, UID + 1, 27, 0)), BASE);
        unrelated.start(false);
        rejects(InvalidForegroundServiceTypeException.class, () -> unrelated.promote(1, 0),
                "An older package under another UID must not weaken type checks");
        Harness ownerP = new Harness(28, 0, List.of(code(sibling, UID, 27, 0)));
        ownerP.start(false);
        rejects(SecurityException.class, () -> ownerP.promote(1, 0),
                "The P base permission gate remains tied to the owning app, not the UID minimum target");
    }
    private static void promotionDeadline() throws Exception {
        Harness ordinary = new Harness(33, 0, BASE);
        ordinary.start(false);
        check(ordinary.promotionDeadline() == null, "Ordinary service start must not arm a deadline");

        Harness legacy = new Harness(25, 0);
        legacy.start(true);
        check(legacy.promotionDeadline() == null, "Pre-O service owner must not owe a promotion");

        Harness promoted = new Harness(33, 0, BASE);
        promoted.start(true);
        Object first = promoted.promotionDeadline();
        check(first != null, "Foreground start must arm a deadline at start-argument delivery");
        ScheduledFuture<?> firstTimer = (ScheduledFuture<?>) get(first, "future");
        check(!firstTimer.isDone(), "New deadline must be queued");
        promoted.start(true);
        check(promoted.promotionDeadline() == first, "Repeated foreground starts must not extend the deadline");
        promoted.promote(7, 0);
        check(promoted.promotionDeadline() == null && firstTimer.isCancelled(),
                "Promotion cancels the original deadline");
        promoted.expire(first);
        check(promoted.thread.crashes == 0, "A queued callback after promotion cannot crash the process");
        promoted.start(true);
        check(promoted.promotionDeadline() == null, "An already-foreground service owes no new promotion");

        Harness replacedStart = new Harness(33, 0, BASE);
        replacedStart.start(true);
        Object replaced = replacedStart.promotionDeadline();
        replacedStart.start(false);
        check(replacedStart.promotionDeadline() == null, "A later ordinary start supersedes the foreground requirement");
        replacedStart.start(true);
        Object second = replacedStart.promotionDeadline();
        check(second != null && second != replaced, "A later foreground start may arm a different deadline");
        replacedStart.expire(replaced);
        check(replacedStart.promotionDeadline() == second && replacedStart.thread.crashes == 0,
                "The canceled deadline cannot expire the replacement");
        call(replacedStart.services, "stopPackage", new Class<?>[]{String.class}, OWNER);
        replacedStart.expire(second);
        check(replacedStart.promotionDeadline() == null && replacedStart.thread.crashes == 0,
                "Package-wide stop must discard its deadlines without a late crash");

        Harness timedOut = new Harness(33, 0, BASE);
        timedOut.start(true);
        Object expired = timedOut.promotionDeadline();
        timedOut.expire(expired);
        timedOut.expire(expired);
        check(timedOut.thread.crashes == 1, "The exact deadline reports failure only once");
        check(!((Boolean) get(timedOut.thread.service, "startRequested")),
                "The timed-out service must no longer be started");
        check(timedOut.thread.crashType == ForegroundServiceDidNotStartInTimeException.TYPE_ID
                        && timedOut.thread.crashMessage.contains(SERVICE.flattenToShortString())
                        && SERVICE.getClassName().equals(ForegroundServiceDidNotStartInTimeException
                                .getServiceClassNameFromExtras(timedOut.thread.crashExtras)),
                "The typed app-thread error must name the original service");

        Harness bound = new Harness(33, 0, BASE);
        bound.start(true);
        Object beforeBoundTimeout = bound.promotionDeadline();
        IServiceConnection connection = new IServiceConnection.Stub() {
            @Override public void connected(ComponentName name, IBinder service, IBinderSession session,
                    boolean dead) { }
        };
        bound.asApp(() -> check(bound.am.bindService(bound.thread, null,
                new Intent().setComponent(SERVICE), null, connection, Context.BIND_AUTO_CREATE,
                OWNER, 0) == 1, "Auto-create connection must keep the service instance"));
        bound.expire(beforeBoundTimeout);
        check(!((Boolean) get(bound.thread.service, "startRequested")) && bound.thread.crashes == 0,
                "A timed-out started lifetime must end while its binding keeps the instance");
        bound.asApp(() -> check(bound.am.unbindService(connection), "Auto-create client can unbind"));
        check(bound.thread.crashes == 1, "Tearing down the bound instance reports the missed promotion");

        Harness stopped = new Harness(33, 0, BASE);
        stopped.start(true);
        Object beforeStop = stopped.promotionDeadline();
        stopped.stop();
        stopped.expire(beforeStop);
        check(stopped.thread.crashes == 1 && stopped.promotionDeadline() == null,
                "Explicit stop before promotion reports the failure once and removes the timer");

        Harness died = new Harness(33, 0, BASE);
        died.start(true);
        Object beforeDeath = died.promotionDeadline();
        set(died.process, "exiting", true);
        call(died.services, "processDied", new Class<?>[]{died.process.getClass()}, died.process);
        died.expire(beforeDeath);
        check(died.promotionDeadline() == null && died.thread.crashes == 0,
                "Process death invalidates the previous attachment's timer");
    }

    public static int run(boolean parcel, String scenario) throws Exception {
        binder = parcel; checks = 0;
        try (FixtureServiceContext.Scope owned = new FixtureServiceContext.Scope()) {
            contexts = owned;
            if (scenario.equals("all") || scenario.equals("base")) basePermission();
            if (scenario.equals("all") || scenario.equals("types")) typeBoundaries();
            if (scenario.equals("all") || scenario.equals("permissions")) manifestPermissions();
            if (scenario.equals("all") || scenario.equals("normalization")) normalizationAndStop();
            if (scenario.equals("all") || scenario.equals("shared-uid")) sharedUidTargets();
            if (scenario.equals("all") || scenario.equals("deadline")) promotionDeadline();
        } finally { contexts = null; }
        return checks;
    }
    public static void main(String[] args) throws Exception {
        System.out.println("ForegroundServiceAdmissionTest all passed: " + run(true, "all") + " checks; binder=true");
    }
}
