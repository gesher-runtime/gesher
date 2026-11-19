/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IApplicationThread;
import android.app.INotificationManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PropertyInvalidatedCache;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ParceledListSlice;
import android.content.pm.PermissionInfo;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import android.service.notification.StatusBarNotification;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.ActivityManagerService;
import com.gesher.server.ActivityTaskManagerService;
import com.gesher.server.CallerIdentity;
import com.gesher.server.GmsSignatureSpoofing;
import com.gesher.server.NotificationManagerService;
import com.gesher.server.PackageManagerService;
import com.gesher.server.StandIn;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.time.InstantSource;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.ToIntBiFunction;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import sun.misc.Unsafe;

/** Real notification/caller/permission policy with explicitly constructed package/process metadata. */
public final class NotificationDelegationTest {
    private static final String PREFIX = "fixture.notificationdelegation.";
    private static final String OWNER = PREFIX + "owner", DELEGATE = PREFIX + "delegate";
    private static final String OTHER = PREFIX + "other", SHARED = PREFIX + "shared";
    private static final String POST = "android.permission.POST_NOTIFICATIONS";
    private static final Unsafe UNSAFE;
    private static int checks;
    private static boolean parcel;
    private static boolean storage;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Work { void run() throws Exception; }
    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }
    private static void rejects(Class<? extends Throwable> type, Work work, String reason) throws Exception {
        try { work.run(); }
        catch (Throwable error) {
            if (error instanceof InvocationTargetException) error = error.getCause();
            if (type.isInstance(error)) { checks++; return; }
            throw new AssertionError(reason + ": wrong failure", error);
        }
        throw new AssertionError(reason + ": no exception");
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field value = type.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }
    private static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Object make(Class<?> cls, Class<?>[] types, Object... args) throws Exception {
        var constructor = cls.getDeclaredConstructor(types); constructor.setAccessible(true);
        return constructor.newInstance(args);
    }
    private static Object invoke(Class<?> cls, Object target, String name, Class<?>[] types, Object... args) throws Exception {
        Method method = cls.getDeclaredMethod(name, types); method.setAccessible(true);
        try { return method.invoke(target, args); }
        catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception e) throw e;
            if (failure.getCause() instanceof Error e) throw e;
            throw failure;
        }
    }
    private static IBinder token() {
        if (parcel) return new Binder();
        return (IBinder)Proxy.newProxyInstance(IBinder.class.getClassLoader(), new Class<?>[]{IBinder.class},
                (self, method, args) -> switch (method.getName()) {
                    case "equals" -> self == args[0];
                    case "hashCode" -> System.identityHashCode(self);
                    case "toString" -> "Isolated notification host identity";
                    default -> throw new AssertionError("Unexpected host transport " + method.getName());
                });
    }
    private static final class AppThread extends IApplicationThread.Default {
        private final IBinder token = token();
        @Override public IBinder asBinder() { return token; }
    }
    private static ApkPackage code(String name, int uid, int version, String signedApk) throws Exception {
        File root = new File(System.getenv("ANDROID_DATA"), "code/" + name + "/" + version);
        Files.createDirectories(root.toPath()); File apk = new File(root, "base.apk");
        if (signedApk != null) Files.copy(new File(signedApk).toPath(), apk.toPath());
        else try (ZipOutputStream zip = new ZipOutputStream(Files.newOutputStream(apk.toPath()))) {
            zip.putNextEntry(new ZipEntry("fixture.txt"));
            zip.write((name + ":" + version).getBytes(java.nio.charset.StandardCharsets.UTF_8)); zip.closeEntry();
        }
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, apk.getPath());
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        parsed.addUsesPermission(new ParsedUsesPermissionImpl(POST, 0, java.util.Collections.emptySet()));
        parsed.hideAsFinal();
        ApkPackage code = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(code, "packageName", name); set(code, "parsed", parsed); set(code, "mUid", uid);
        set(code, "apkPath", apk.getPath()); set(code, "mDataDir", new File(root, "data").getPath());
        set(code, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        return code;
    }
    private static final class Harness {
        final boolean protectedMode = GmsSignatureSpoofing.isEnabled();
        final Map<String, ApkPackage> packages = new LinkedHashMap<>();
        final Map<String, Object> processes = new LinkedHashMap<>();
        final PackageManagerService pm;
        final ActivityTaskManagerService atm;
        final ActivityManagerService am;
        final AtomicInteger publications = new AtomicInteger();
        NotificationManagerService service;
        INotificationManager wire;
        Harness() throws Exception {
            packages.put(OWNER, code(OWNER, protectedMode ? 13201 : 10000, 1, null));
            packages.put(DELEGATE, code(DELEGATE, protectedMode ? 13202 : 10000, 1, null));
            packages.put(OTHER, code(OTHER, protectedMode ? 13203 : 10000, 1, null));
            packages.put(SHARED, code(SHARED, packages.get(OWNER).uid(), 1, null));
            if (parcel) pm = new PackageManagerService(packages.values());
            else {
                // Skip the package service's unrelated Binder callback registration
                // on the host; retain its actual package and installation-owner logic.
                pm = (PackageManagerService)UNSAFE.allocateInstance(PackageManagerService.class);
                set(pm, "mPackages", new LinkedHashMap<>(packages));
                set(pm, "mRemoving", new LinkedHashMap<>());
                set(pm, "mPackageRemoval", new ArrayList<>());
                set(pm, "mPackageRemovalChecks", new ArrayList<>());
                set(pm, "mPermissions", make(type("PermissionPolicy"), new Class<?>[]{PackageManagerService.class}, pm));
                invoke(PackageManagerService.class, pm, "enabledStore", new Class<?>[]{});
            }
            atm = (ActivityTaskManagerService)UNSAFE.allocateInstance(ActivityTaskManagerService.class);
            set(atm, "mPm", pm); set(atm, "mActivities", new ArrayList<>());
            List<Object> attached = new ArrayList<>(); set(atm, "mProcesses", attached);
            int pid = 893001;
            for (ApkPackage pkg : packages.values()) {
                Object process = UNSAFE.allocateInstance(type("ActivityTaskManagerService$ProcessRecord"));
                set(process, "pid", pid++); set(process, "uid", pkg.uid());
                set(process, "packageName", pkg.packageName); set(process, "processName", pkg.packageName);
                set(process, "thread", new AppThread()); set(process, "ready", true);
                processes.put(pkg.packageName, process); attached.add(process);
            }
            am = (ActivityManagerService)UNSAFE.allocateInstance(ActivityManagerService.class);
            set(am, "mPm", pm); set(am, "mAtm", atm);
            CallerIdentity identity = new CallerIdentity(pm, atm); set(am, "mCallers", identity);
            invoke(pm.permissions().getClass(), pm.permissions(), "attach", new Class<?>[]{CallerIdentity.class}, identity);
            if (!parcel) {
                // The host cannot parse framework-res through Android's asset JNI.
                // Only permission metadata is supplied; real PermissionPolicy decides grants.
                Object catalog = get(pm.permissions(), "mCatalog");
                PermissionInfo post = new PermissionInfo(); post.name = POST; post.packageName = "android";
                post.protectionLevel = PermissionInfo.PROTECTION_DANGEROUS;
                ((Map<String, PermissionInfo>)get(catalog, "mFramework")).put(POST, post);
                set(catalog, "mLoaded", true);
            }
            restart();
            for (String name : packages.keySet()) {
                permission(name, true);
                as(name, () -> wire.createNotificationChannels(name, new ParceledListSlice<>(List.of(
                        new NotificationChannel("delegation", "Delegation", NotificationManager.IMPORTANCE_LOW)))));
            }
        }
        void restart() throws Exception {
            ToIntBiFunction<StatusBarNotification, Boolean> sink = (value, update) -> { publications.incrementAndGet(); return 0; };
            service = (NotificationManagerService)make(NotificationManagerService.class,
                    new Class<?>[]{PackageManagerService.class, ActivityManagerService.class, ToIntBiFunction.class}, pm, am, sink);
            wire = service;
            if (parcel) {
                StandIn.setStrict(true);
                IBinder target = StandIn.of(INotificationManager.class, service).asBinder();
                wire = INotificationManager.Stub.asInterface(new Binder() {
                    @Override public IInterface queryLocalInterface(String descriptor) { return null; }
                    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                        return target.transact(code, data, reply, flags);
                    }
                });
            }
        }
        NotificationManager manager(String name) {
            Context context = new ContextWrapper(null) {
                @Override public String getPackageName() { return name; }
                @Override public String getOpPackageName() { return name; }
                @Override public int getUserId() { return 0; }
            };
            return new NotificationManager(context, InstantSource.system()) {
                @Override protected INotificationManager service() { return wire; }
            };
        }
        void identity(int pid, int uid, Work work) throws Exception {
            long before = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)uid << 32) | pid);
                check(Binder.getCallingPid() == pid && Binder.getCallingUid() == uid, "Fixture caller identity boundary unavailable");
                work.run();
            } finally { Binder.restoreCallingIdentity(before); }
        }
        void as(String name, Work work) throws Exception {
            Object process = processes.get(name);
            identity((int)get(process, "pid"), (int)get(process, "uid"), work);
        }
        void permission(String name, boolean grant) throws Exception {
            if (grant) pm.permissions().grantRuntimePermission(name, POST, "default:0", 0);
            else pm.permissions().revokeRuntimePermission(name, POST, "default:0", 0, "isolated fixture");
        }
        Notification notification() throws Exception {
            Notification value = new Notification(); field(Notification.class, "mChannelId").set(value, "delegation");
            value.setSmallIcon(android.graphics.drawable.Icon.createWithResource(OWNER, android.R.drawable.ic_dialog_info));
            return value;
        }
        void post(String caller, String owner, int id) throws Exception {
            as(caller, () -> wire.enqueueNotificationWithTag(owner, caller, null, id, notification(), 0));
        }
        void cancel(String caller, String owner, int id) throws Exception {
            as(caller, () -> wire.cancelNotificationWithTag(owner, caller, null, id, 0));
        }
        List<StatusBarNotification> active(String caller) throws Exception {
            List<StatusBarNotification> values = new ArrayList<>();
            as(caller, () -> values.addAll(wire.getAppActiveNotifications(caller, 0).getList())); return values;
        }
        boolean has(String caller, int id) throws Exception { return active(caller).stream().anyMatch(value -> value.getId() == id); }
        void grant(String delegate) throws Exception { as(OWNER, () -> manager(OWNER).setNotificationDelegate(delegate)); }
        boolean allowed(String caller, String owner) throws Exception {
            boolean[] allowed = {false}; as(caller, () -> allowed[0] = manager(caller).canNotifyAsPackage(owner)); return allowed[0];
        }
        String delegate() throws Exception {
            String[] delegate = {null}; as(OWNER, () -> delegate[0] = manager(OWNER).getNotificationDelegate()); return delegate[0];
        }
        void replace(String name, int version, boolean uninstall, String signedApk) throws Exception {
            ApkPackage before = packages.get(name);
            if (uninstall) {
                invoke(NotificationManagerService.class, service, "packageRemoved", new Class<?>[]{String.class}, name);
                invoke(PackageManagerService.class, pm, "remove", new Class<?>[]{String.class}, name);
            }
            ApkPackage replacement = code(name, before.uid(), version, signedApk);
            invoke(PackageManagerService.class, pm, "add", new Class<?>[]{ApkPackage.class}, replacement);
            packages.put(name, replacement);
        }
    }

    private static void ownership(Harness h) throws Exception {
        check(h.delegate() == null, "Fresh source unexpectedly has a delegate");
        check(h.allowed(OWNER, OWNER), "Own-package canNotifyAsPackage denied");
        check(!h.allowed(DELEGATE, OWNER), "Unapproved package can notify");
        rejects(SecurityException.class, () -> h.post(DELEGATE, OWNER, 2), "Unapproved delegated post");
        h.as(OTHER, () -> {
            rejects(SecurityException.class, () -> h.wire.setNotificationDelegate(OWNER, OTHER), "Claimed source package trusted");
            rejects(SecurityException.class, () -> h.wire.getNotificationDelegate(OWNER), "Unowned delegate query");
            rejects(SecurityException.class, () -> h.wire.canNotifyAsPackage(DELEGATE, OWNER, 0), "Claimed caller package trusted");
        });
        h.identity(893999, h.packages.get(DELEGATE).uid(), () ->
                rejects(SecurityException.class, () -> h.wire.canNotifyAsPackage(DELEGATE, OWNER, 0), "Unattached PID trusted"));
        if (h.protectedMode) h.identity((int)get(h.processes.get(DELEGATE), "pid"), 13299, () ->
                rejects(SecurityException.class, () -> h.wire.canNotifyAsPackage(DELEGATE, OWNER, 0), "Wrong Binder UID trusted"));
        h.identity((int)get(h.processes.get(OTHER), "pid"), 1000, () ->
                rejects(SecurityException.class, () -> h.wire.setNotificationDelegate(OWNER, OTHER), "Attached app elevated to system"));
        h.as(DELEGATE, () -> rejects(SecurityException.class,
                () -> h.wire.canNotifyAsPackage(DELEGATE, OWNER, 10), "Unsupported user accepted"));
        h.grant(DELEGATE);
        check(DELEGATE.equals(h.delegate()) && h.allowed(DELEGATE, OWNER), "Approved delegate not authorized");
        check(!h.allowed(OTHER, OWNER), "Approval leaked to another package");
        h.grant(PREFIX + "absent"); check(DELEGATE.equals(h.delegate()), "Unknown delegate replaced valid grant");
        h.cancel(DELEGATE, PREFIX + "absent", 2);
        h.as(DELEGATE, () -> {
            check(h.wire.getNotificationChannel(DELEGATE, 0, OWNER, "delegation") != null, "Delegate cannot query owner channel");
            check(h.wire.getNotificationChannels(DELEGATE, OWNER, 0).getList().size() == 1, "Delegate channel list incorrect");
            rejects(SecurityException.class, () -> h.wire.createNotificationChannels(OWNER, new ParceledListSlice<>(List.of())), "Delegation authorized channel mutation");
        });
        h.as(OTHER, () -> rejects(SecurityException.class,
                () -> h.wire.getNotificationChannel(OTHER, 0, OWNER, "delegation"), "Channel data exposed to nondelegate"));
    }

    private static void posting(Harness h) throws Exception {
        h.post(OWNER, OWNER, 1); h.post(DELEGATE, OWNER, 2);
        check(h.has(OWNER, 1) && h.has(OWNER, 2), "Owner cannot see both notification origins");
        List<StatusBarNotification> values = h.active(DELEGATE).stream()
                .filter(value -> value.getTag() == null).toList();
        check(values.size() == 1 && values.get(0).getId() == 2, "Delegate view includes another poster's notification");
        StatusBarNotification delegated = values.get(0);
        check(OWNER.equals(delegated.getPackageName()) && DELEGATE.equals(delegated.getOpPkg())
                && delegated.getUid() == h.packages.get(OWNER).uid(), "Delegation lost owner/opPkg/UID");
        check(delegated.getInitialPid() == (int)get(h.processes.get(DELEGATE), "pid"), "Posting PID was not retained");
        delegated.getNotification().flags = 0x7fffffff;
        check(h.active(DELEGATE).stream().filter(value -> value.getId() == 2 && value.getTag() == null)
                .findFirst().orElseThrow().getNotification().flags != 0x7fffffff, "Active notification query aliases service state");
        check(h.active(OTHER).isEmpty(), "Unrelated app sees delegated content");
        rejects(SecurityException.class, () -> h.cancel(DELEGATE, OWNER, 1), "Delegate cancelled owner's notification");
        h.cancel(DELEGATE, OWNER, 2); check(!h.has(OWNER, 2) && h.has(OWNER, 1), "Delegate cancellation removed wrong record");
        h.post(DELEGATE, OWNER, 3); h.post(OWNER, OWNER, 3);
        rejects(SecurityException.class, () -> h.cancel(DELEGATE, OWNER, 3), "Delegate cancelled owner replacement");
        h.post(DELEGATE, OWNER, 4); h.grant(null);
        check(h.delegate() == null && !h.allowed(DELEGATE, OWNER), "Revocation failed");
        check(h.has(DELEGATE, 4) && h.has(OWNER, 4), "Revocation removed or hid an existing delegated post");
        rejects(SecurityException.class, () -> h.post(DELEGATE, OWNER, 5), "Revoked delegate posted");
        rejects(SecurityException.class, () -> h.cancel(DELEGATE, OWNER, 4), "Revoked delegate cancelled");
        h.as(DELEGATE, () -> rejects(SecurityException.class,
                () -> h.wire.getNotificationChannel(DELEGATE, 0, OWNER, "delegation"), "Revoked delegate read owner channel"));
        h.cancel(OWNER, OWNER, 4); check(!h.has(DELEGATE, 4), "Owner cannot cancel delegated content");
        h.grant(DELEGATE); h.permission(DELEGATE, false);
        h.post(DELEGATE, OWNER, 6); check(h.has(OWNER, 6), "Delegate's own POST permission incorrectly gates owner publication");
        if (h.protectedMode) {
            h.permission(OWNER, false); int before = h.publications.get();
            h.post(DELEGATE, OWNER, 7);
            check(h.publications.get() == before && h.active(OWNER).isEmpty(), "Owner permission denial leaked active content or publication");
            h.permission(OWNER, true);
        }
        check(!h.allowed(SHARED, OWNER), "Same UID alone changed canNotifyAsPackage contract");
        h.as(SHARED, () -> rejects(SecurityException.class,
                () -> h.wire.getNotificationChannel(SHARED, 0, OWNER, "delegation"),
                "Same UID alone authorized a cross-package channel query"));
        if (h.protectedMode) {
            h.post(SHARED, OWNER, 30); check(h.has(OWNER, 30), "Authenticated shared UID cannot post for its co-owner");
            h.as(SHARED, () -> check(h.wire.getNotificationDelegate(OWNER).equals(DELEGATE), "Same UID cannot query owned source"));
            h.post(OWNER, OWNER, 31);
            rejects(SecurityException.class, () -> h.cancel(SHARED, OWNER, 31), "Shared UID ignored original-poster cancellation check");
        } else rejects(SecurityException.class, () -> h.post(SHARED, OWNER, 30), "Legacy shared UID10000 incorrectly confers ownership");
    }

    private static void lifecycle(Harness h, String signedApk) throws Exception {
        h.restart(); check(DELEGATE.equals(h.delegate()) && h.allowed(DELEGATE, OWNER), "Grant did not survive service restart");
        File blocked = new File(System.getenv("ANDROID_DATA"), "system/notification-delegates/" + OWNER + ".delegate.new");
        Files.createDirectory(blocked.toPath());
        try { rejects(IllegalStateException.class, () -> h.grant(OTHER), "Failed durable write reported successful grant"); }
        finally { Files.delete(blocked.toPath()); }
        check(DELEGATE.equals(h.delegate()) && !h.allowed(OTHER, OWNER), "Failed durable write published new delegate");
        h.restart(); check(DELEGATE.equals(h.delegate()), "Failed write destroyed previous persisted grant");
        h.replace(DELEGATE, 2, true, null);
        check(DELEGATE.equals(h.delegate()) && !h.allowed(DELEGATE, OWNER), "Reinstalled delegate inherited grant at retained UID");
        h.grant(DELEGATE); check(h.allowed(DELEGATE, OWNER), "Owner cannot explicitly regrant new installation");
        h.replace(DELEGATE, 3, false, null);
        check(!h.allowed(DELEGATE, OWNER), "Changed unsigned code inherited delegation through unchanged install owner");
        h.grant(DELEGATE); check(h.allowed(DELEGATE, OWNER), "Unsigned replacement cannot receive explicit new grant");
        if (signedApk != null) {
            h.replace(DELEGATE, 4, true, signedApk);
            check(h.packages.get(DELEGATE).hasVerifiedSigningIdentity(), "Signed continuity fixture lacks real verified APK bytes");
            h.grant(DELEGATE); h.replace(DELEGATE, 5, false, signedApk);
            check(h.allowed(DELEGATE, OWNER), "Verified same-signer update lost grant with retained installation owner");
        }
        h.replace(OWNER, 2, true, null);
        check(h.delegate() == null && !h.allowed(DELEGATE, OWNER), "Reinstalled owner restored its old grant");
        h.grant(DELEGATE); h.grant(null); h.restart();
        check(h.delegate() == null && !h.allowed(DELEGATE, OWNER), "Revocation was not durable");
    }

    /** Real Parcel storage migration and original-poster authorization after server restart. */
    private static void retained(Harness h) throws Exception {
        if (!storage) return;
        Field last = field(type("Services"), "sLast"); Object before = last.get(null);
        last.set(null, make(type("Services$Created"),
                new Class<?>[]{PackageManagerService.class, ActivityTaskManagerService.class, ActivityManagerService.class},
                h.pm, h.atm, h.am));
        Class<?> store = type("NotificationStore");
        try {
            h.grant(DELEGATE);
            Notification notification = h.notification();
            String key = new StatusBarNotification(OWNER, DELEGATE, 80, null, h.packages.get(OWNER).uid(),
                    0, notification, android.os.UserHandle.SYSTEM, null, 0).getKey();
            invoke(store, null, "save", new Class<?>[]{ActivityManagerService.class, String.class, String.class,
                    String.class, int.class, Notification.class, String.class}, null, key, OWNER, null, 80, notification, DELEGATE);
            Object stored = invoke(store, null, "load", new Class<?>[]{String.class, String.class}, OWNER, key);
            check(stored != null && DELEGATE.equals(get(stored, "opPkg")), "Retained current state lost actual poster");
            h.restart(); h.grant(OTHER);
            rejects(SecurityException.class, () -> h.cancel(OTHER, OWNER, 80), "New delegate cancelled prior delegate's retained post");
            h.grant(DELEGATE); h.cancel(DELEGATE, OWNER, 80);
            check(invoke(store, null, "load", new Class<?>[]{String.class, String.class}, OWNER, key) == null,
                    "Original delegate cannot cancel its retained post");
            // Version 3 had no delegated posting; its only valid poster is the owner.
            Parcel data = Parcel.obtain();
            try {
                data.writeInt(3); data.writeString(key); data.writeString(OWNER);
                data.writeString(h.packages.get(OWNER).codeIdentity()); data.writeString(null);
                data.writeInt(80); data.writeInt(0); data.writeString("delegation");
                data.writeInt(0); data.writeInt(0);
                File directory = new File(System.getenv("ANDROID_DATA"), "system/notifications/" + OWNER);
                Files.createDirectories(directory.toPath());
                Files.write(new File(directory, Integer.toHexString(key.hashCode()) + ".n").toPath(), data.marshall());
            } finally { data.recycle(); }
            stored = invoke(store, null, "load", new Class<?>[]{String.class, String.class}, OWNER, key);
            check(stored != null && OWNER.equals(get(stored, "opPkg")), "Version-3 record did not retain owner-only posting identity");
            rejects(SecurityException.class, () -> h.cancel(DELEGATE, OWNER, 80), "Delegate cancelled older owner-only retained state");
            h.cancel(OWNER, OWNER, 80);
            check(invoke(store, null, "load", new Class<?>[]{String.class, String.class}, OWNER, key) == null,
                    "Owner cannot remove its migrated retained state");
            // The independently published v4 formats have different attribution.
            // Decode their original bytes, restart, and enforce actual cancellation
            // policy before the Windows removal boundary sees any identity.
            for (boolean delegatedLayout : new boolean[]{true, false}) {
                String current = (String) invoke(NotificationManagerService.class, null, "toastTag",
                        new Class<?>[]{String.class, int.class}, null, 80);
                Parcel historical = Parcel.obtain();
                try {
                    historical.writeInt(4); historical.writeString(key); historical.writeString(OWNER);
                    historical.writeString(h.packages.get(OWNER).codeIdentity()); historical.writeString(null);
                    historical.writeInt(80); historical.writeInt(0); historical.writeString("delegation");
                    if (delegatedLayout) historical.writeString(DELEGATE);
                    else { historical.writeString(current); historical.writeString("80"); }
                    historical.writeInt(0); historical.writeInt(0);
                    File directory = new File(System.getenv("ANDROID_DATA"), "system/notifications/" + OWNER);
                    Files.createDirectories(directory.toPath());
                    Files.write(new File(directory, Integer.toHexString(key.hashCode()) + ".n").toPath(), historical.marshall());
                } finally { historical.recycle(); }
                h.restart();
                stored = invoke(store, null, "load", new Class<?>[]{String.class, String.class}, OWNER, key);
                check(stored != null && (delegatedLayout ? DELEGATE : OWNER).equals(get(stored, "opPkg")),
                        "Historical v4 layout changed original poster");
                Map<String, String> history = new LinkedHashMap<>();
                history.put(current, key); history.put("80", "different-retained-owner");
                AtomicInteger removals = new AtomicInteger();
                set(h.service, "mTestToastRemoval", (ToIntBiFunction<String, String>) (tag, expected) -> {
                    removals.incrementAndGet();
                    if (expected != null && history.containsKey(tag) && !expected.equals(history.get(tag))) return 1;
                    history.remove(tag); return 0;
                });
                h.grant(OTHER);
                rejects(SecurityException.class, () -> h.cancel(OTHER, OWNER, 80),
                        "New delegate cancelled historical v4 state it did not post");
                check(removals.get() == 0 && history.size() == 2, "Unauthorized cancellation reached toast removal");
                h.grant(DELEGATE);
                if (delegatedLayout) h.cancel(DELEGATE, OWNER, 80);
                else {
                    rejects(SecurityException.class, () -> h.cancel(DELEGATE, OWNER, 80),
                            "Toast-only v4 invented delegated ownership");
                    check(removals.get() == 0, "Owner-only record leaked into delegated Windows removal");
                    h.cancel(OWNER, OWNER, 80);
                }
                check(!history.containsKey(current) && "different-retained-owner".equals(history.get("80")),
                        "Authenticated migration cancellation removed a colliding legacy owner");
                check(invoke(store, null, "load", new Class<?>[]{String.class, String.class}, OWNER, key) == null,
                        "Authorized historical v4 cancellation retained its record");
            }
        } finally { last.set(null, before); }
    }

    public static int run(boolean useParcel, String signedApk) throws Exception {
        return run(useParcel, useParcel, signedApk);
    }

    public static int run(boolean useParcel, boolean useStorage, String signedApk) throws Exception {
        parcel = useParcel; storage = useStorage; checks = 0;
        PropertyInvalidatedCache.setTestMode(true);
        Harness h = new Harness();
        ownership(h); posting(h); retained(h); lifecycle(h, signedApk);
        return checks;
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false, System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File parent = new File(System.getenv("ANDROID_DATA")); Files.createDirectories(parent.toPath());
        File data = Files.createTempDirectory(parent.toPath(), "notification-delegation-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        System.out.println("NotificationDelegationTest all passed: " + run(true, null) + " checks");
        System.out.println("Real ART/framework/Binder Parcel policy; constructed manifest/process identities and explicit toast sink, not Windows delivery.");
    }
}
