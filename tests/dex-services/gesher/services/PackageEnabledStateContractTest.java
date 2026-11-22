/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ApplicationPackageManager;
import android.app.IApplicationThread;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.UserHandle;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.android.internal.pm.pkg.component.ParsedIntentInfoImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;
import com.gesher.server.StandIn;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executor;
import java.util.concurrent.Delayed;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import sun.misc.Unsafe;

/** Real service, metadata, resolver and public manager; ART also uses the AIDL Parcel path. */
public final class PackageEnabledStateContractTest {
    private static final Unsafe UNSAFE;
    private static final int KEEP = PackageManager.DONT_KILL_APP | PackageManager.SYNCHRONOUS;
    private static int checks, transactions;
    private static boolean art;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Call { void run() throws Exception; }
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason); checks++;
    }
    private static void expect(Class<? extends Throwable> type, Call call, String reason) throws Exception {
        try { call.run(); }
        catch (Throwable failure) {
            if (failure instanceof InvocationTargetException wrapped) failure = wrapped.getCause();
            if (!type.isInstance(failure)) throw new AssertionError(reason + ": " + failure, failure);
            checks++; return;
        }
        throw new AssertionError(reason + ": no exception");
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }
    private static Object invoke(Object target, String name, Class<?>[] types, Object... args) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name, types); method.setAccessible(true);
        return method.invoke(target, args);
    }
    private static ParsedIntentInfoImpl filter(String name) {
        ParsedIntentInfoImpl info = new ParsedIntentInfoImpl();
        info.getIntentFilter().addAction(name + ".ACTION");
        info.getIntentFilter().addCategory(Intent.CATEGORY_DEFAULT); info.setHasDefault(true);
        return info;
    }
    private static ApkPackage record(String name, boolean manifestEnabled) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(manifestEnabled); parsed.setTargetSdkVersion(36);
        for (String suffix : new String[]{"Main", "Alias"}) {
            ParsedActivityImpl activity = new ParsedActivityImpl();
            activity.setName(name + "." + suffix); activity.setPackageName(name);
            activity.setEnabled(suffix.equals("Main")); activity.setExported(true);
            activity.addIntent(filter(name));
            if (suffix.equals("Alias")) activity.setTargetActivity(name + ".Main");
            parsed.addActivity(activity);
        }
        ParsedActivityImpl receiver = new ParsedActivityImpl();
        receiver.setName(name + ".Receiver"); receiver.setPackageName(name); receiver.setEnabled(true);
        receiver.addIntent(filter(name));
        parsed.addReceiver(receiver);
        ParsedServiceImpl service = new ParsedServiceImpl();
        service.setName(name + ".Service"); service.setPackageName(name); service.setEnabled(true);
        service.addIntent(filter(name));
        parsed.addService(service);
        ParsedProviderImpl provider = new ParsedProviderImpl();
        provider.setName(name + ".Provider"); provider.setPackageName(name); provider.setEnabled(true);
        provider.addIntent(filter(name));
        provider.setAuthority(name + ".items"); parsed.addProvider(provider); parsed.hideAsFinal();
        ApkPackage result = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(result, "packageName", name); set(result, "parsed", parsed); set(result, "apkPath", parsed.getBaseApkPath());
        set(result, "mDataDir", new File(System.getenv("ANDROID_DATA"), "data/" + name).getPath());
        set(result, "mUid", 12345);
        set(result, "sharedLibraries", field(SharedLibraryGraph.class, "EMPTY").get(null));
        return result;
    }
    private static ComponentName component(String name, String suffix) { return new ComponentName(name, name + "." + suffix); }
    private static PackageManager.ComponentEnabledSetting app(String name, int value) {
        return new PackageManager.ComponentEnabledSetting(name, value, KEEP);
    }
    private static PackageManager.ComponentEnabledSetting part(String name, String suffix, int value, int flags) {
        return new PackageManager.ComponentEnabledSetting(component(name, suffix), value, flags);
    }
    private static final class Harness {
        final PackageManagerService service;
        final IPackageManager api;
        final PackageManager client;
        final List<Runnable> tasks = new ArrayList<>();
        final List<Intent> events = new ArrayList<>();
        final List<ApkPackage> kills = new ArrayList<>();
        Harness(ApkPackage... records) throws Exception { this(true, records); }
        Harness(boolean persistent, ApkPackage... records) throws Exception {
            if (persistent) service = (PackageManagerService)UNSAFE.allocateInstance(PackageManagerService.class);
            else {
                var constructor = PackageManagerService.class.getDeclaredConstructor(java.util.Collection.class, boolean.class);
                constructor.setAccessible(true);
                service = constructor.newInstance(Arrays.asList(records), false);
            }
            Map<String, ApkPackage> packages = new LinkedHashMap<>();
            for (ApkPackage record : records) packages.put(record.packageName, record);
            set(service, "mPackages", packages); set(service, "mRemoving", Map.of());
            set(service, "mPackageEventExecutor", (Executor)tasks::add);
            set(service, "mPackageEventSender", (Consumer<Intent>)intent -> {
                check(!Thread.holdsLock(service), "Event callback held the package monitor");
                events.add(intent);
            });
            // Unsafe allocation skips the listener list's field initializer.
            if (persistent) {
                try {
                    set(service, "mPackageEventListeners", new CopyOnWriteArrayList<Consumer<Intent>>());
                } catch (NoSuchFieldException oldImplementation) { }
            }
            // Keep the public-API fixture runnable against the unchanged baseline.
            try { set(service, "mEnabledStateKiller", (Consumer<ApkPackage>)kills::add); }
            catch (NoSuchFieldException oldImplementation) { }
            Class<?> policy = Class.forName("com.gesher.server.PermissionPolicy");
            var constructor = policy.getDeclaredConstructor(PackageManagerService.class);
            constructor.setAccessible(true); set(service, "mPermissions", constructor.newInstance(service));
            try { invoke(service, "enabledStore", new Class<?>[0]); }
            catch (NoSuchMethodException oldImplementation) { }
            if (art) {
                IBinder target = StandIn.of(IPackageManager.class, service).asBinder();
                Binder transport = new Binder() {
                    @Override public IInterface queryLocalInterface(String descriptor) { return null; }
                    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                        transactions++; return target.transact(code, data, reply, flags);
                    }
                };
                api = IPackageManager.Stub.asInterface(transport);
            } else api = service;
            Object context = UNSAFE.allocateInstance(Class.forName("android.app.ContextImpl"));
            set(context, "mUser", UserHandle.SYSTEM); set(context, "mOpPackageName", records[0].packageName);
            set(context, "mAttributionSource", new android.content.AttributionSource(12345, records[0].packageName, null));
            client = (PackageManager)UNSAFE.allocateInstance(ApplicationPackageManager.class);
            set(client, "mContext", context); set(client, "mPM", api);
        }
        void drain() { while (!tasks.isEmpty()) tasks.remove(0).run(); }
    }

    private static void applicationBatch() throws Exception {
        String a = "fixture.enabled.application";
        Harness h = new Harness(record(a, true));
        h.client.setComponentEnabledSettings(List.of(app(a, 3), part(a, "Alias", 1, KEEP)));
        check(h.client.getApplicationEnabledSetting(a) == 3,
                "Application entry in public batch was ignored");
        check(h.client.getComponentEnabledSetting(component(a, "Alias")) == 1,
                "Mixed application batch lost component entry");
        h.drain();
    }

    private static void metadata() throws Exception {
        String a = "fixture.enabled.metadata", b = "fixture.enabled.manifest";
        Harness h = new Harness(record(a, true), record(b, false));
        Intent implicit = new Intent(a + ".ACTION").setPackage(a);
        check(h.api.queryIntentActivities(implicit, null, 0, 0).getList().size() == 1
                && h.api.queryIntentActivityOptions(null, null, null, implicit, null, 0, 0).getList().size() == 1,
                "Enabled implicit activity/option did not resolve");
        check(h.api.getActivityInfo(component(b, "Main"), 0, 0) == null, "Manifest-disabled app resolved");
        h.client.setApplicationEnabledSetting(a, PackageManager.COMPONENT_ENABLED_STATE_DISABLED_USER, KEEP);
        check(h.client.getApplicationEnabledSetting(a) == 3, "Public application state was ignored");
        check(!h.api.getApplicationInfo(a, 0, 0).enabled && h.api.getApplicationInfo(a, 0, 0).enabledSetting == 3,
                "Application metadata lost its installed disabled state");
        check(h.api.getActivityInfo(component(a, "Main"), 0, 0) == null
                && h.api.getServiceInfo(component(a, "Service"), 0, 0) == null
                && h.api.getReceiverInfo(component(a, "Receiver"), 0, 0) == null
                && h.api.resolveContentProvider(a + ".items", 0, 0) == null,
                "Disabled application remained reachable through a component family");
        check(h.api.queryIntentActivities(implicit, null, 0, 0).getList().isEmpty()
                && h.api.queryIntentServices(implicit, null, 0, 0).getList().isEmpty()
                && h.api.queryIntentReceivers(implicit, null, 0, 0).getList().isEmpty()
                && h.api.queryIntentContentProviders(implicit, null, 0, 0).getList().isEmpty()
                && h.api.queryIntentActivityOptions(null, null, null, implicit, null, 0, 0).getList().isEmpty(),
                "Disabled application remained reachable through an implicit resolver");
        var included = h.api.getActivityInfo(component(a, "Main"), PackageManager.MATCH_DISABLED_COMPONENTS, 0);
        check(included != null && included.enabled && !included.applicationInfo.enabled,
                "MATCH_DISABLED_COMPONENTS lost component/application distinction");
        h.client.setComponentEnabledSetting(component(a, "Alias"), 1, KEEP);
        check(h.api.getActivityInfo(component(a, "Alias"), 0, 0) == null, "Component override bypassed disabled application");
        h.client.setApplicationEnabledSetting(a, 1, KEEP);
        check(h.api.getActivityInfo(component(a, "Alias"), 0, 0) != null, "Enabling application erased component override");
        h.client.setApplicationEnabledSetting(b, 1, KEEP);
        check(h.api.getActivityInfo(component(b, "Main"), 0, 0) != null, "ENABLED did not override application manifest");
        h.client.setApplicationEnabledSetting(b, 0, KEEP);
        check(h.api.getActivityInfo(component(b, "Main"), 0, 0) == null, "DEFAULT did not restore manifest disabled state");
        h.client.setApplicationEnabledSetting(a, 4, KEEP);
        check(h.api.getActivityInfo(component(a, "Main"), 0, 0) == null, "DISABLED_UNTIL_USED was always visible");
        var until = h.api.getActivityInfo(component(a, "Main"), PackageManager.MATCH_DISABLED_UNTIL_USED_COMPONENTS, 0);
        check(until != null && until.applicationInfo.enabled && until.applicationInfo.enabledSetting == 4,
                "Disabled-until-used query flags or enabledSetting were lost");
        h.client.setApplicationEnabledSetting(b, 4, KEEP);
        check(h.api.getActivityInfo(component(b, "Main"), PackageManager.MATCH_DISABLED_UNTIL_USED_COMPONENTS, 0) == null,
                "Disabled-until-used bypassed manifest-disabled application");
        h.drain(); check(h.kills.isEmpty(), "DONT_KILL_APP killed a process");
        Intent event = h.events.get(0);
        check(event.getAction().equals(Intent.ACTION_PACKAGE_CHANGED) && event.getIntExtra(Intent.EXTRA_UID, 0) == 12345
                && event.getStringExtra(Intent.EXTRA_CHANGED_COMPONENT_NAME).equals(a)
                && event.getBooleanExtra(Intent.EXTRA_DONT_KILL_APP, false)
                && (event.getFlags() & Intent.FLAG_RECEIVER_REGISTERED_ONLY) == 0,
                "Application PACKAGE_CHANGED payload/receiver flags were incorrect");
    }

    private static void batches() throws Exception {
        String a = "fixture.enabled.batch";
        Harness h = new Harness(record(a, true));
        h.client.setComponentEnabledSettings(List.of(part(a, "Main", 2, KEEP), part(a, "Alias", 1, KEEP)));
        check(h.client.getComponentEnabledSetting(component(a, "Main")) == 2
                && h.client.getComponentEnabledSetting(component(a, "Alias")) == 1, "Component batch failed");
        h.drain(); check(h.events.size() == 1 && h.kills.isEmpty(), "Batch did not aggregate its event/kill flag");
        Intent event = h.events.get(0);
        check(Arrays.equals(event.getStringArrayExtra(Intent.EXTRA_CHANGED_COMPONENT_NAME_LIST), new String[]{a + ".Main", a + ".Alias"})
                && (event.getFlags() & Intent.FLAG_RECEIVER_REGISTERED_ONLY) != 0, "Component PACKAGE_CHANGED payload incorrect");
        h.client.setComponentEnabledSettings(List.of(app(a, 3), part(a, "Main", 0, KEEP)));
        check(h.client.getApplicationEnabledSetting(a) == 3 && h.client.getComponentEnabledSetting(component(a, "Main")) == 0,
                "Mixed application/component batch dropped an entry");
        h.client.setApplicationEnabledSetting(a, 0, KEEP);
        h.client.setComponentEnabledSetting(component(a, "Alias"), 0, 0);
        h.drain(); check(h.kills.size() == 1, "Changed flags=0 did not request process restart exactly once");
        int oldEvents = h.events.size();
        h.client.setComponentEnabledSetting(component(a, "Alias"), 0, 0);
        h.drain(); check(h.events.size() == oldEvents && h.kills.size() == 1, "No-op emitted kill/broadcast side effects");
        // The real metadata generator must see exactly one active launcher throughout a batch flip.
        AtomicBoolean done = new AtomicBoolean(); AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread reader = new Thread(() -> {
            try {
                while (!done.get()) {
                    PackageInfo info = h.service.getPackageInfo(a, PackageManager.GET_ACTIVITIES, 0);
                    if (info.activities == null || info.activities.length != 1) throw new AssertionError("Partial enabled-state snapshot");
                }
            } catch (Throwable t) { failure.set(t); }
        });
        reader.start();
        try {
            for (int i = 0; i < 40; i++) {
                h.client.setComponentEnabledSettings(List.of(part(a, "Main", (i & 1) == 0 ? 2 : 1, KEEP),
                        part(a, "Alias", (i & 1) == 0 ? 1 : 2, KEEP)));
            }
        } finally { done.set(true); reader.join(10000); }
        check(!reader.isAlive() && failure.get() == null, "Atomic metadata publication failed: " + failure.get());
        h.drain();
    }

    private static void multiplePackages() throws Exception {
        String a = "fixture.enabled.atomicone", b = "fixture.enabled.atomictwo";
        Harness h = new Harness(record(a, true), record(b, true));
        h.client.setComponentEnabledSettings(List.of(app(a, 1), app(b, 2)));
        AtomicBoolean done = new AtomicBoolean(); AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread reader = new Thread(() -> {
            try {
                while (!done.get()) {
                    int active = 0;
                    for (Object value : h.service.getInstalledPackages(0, 0).getList()) {
                        PackageInfo info = (PackageInfo)value;
                        if ((info.packageName.equals(a) || info.packageName.equals(b)) && info.applicationInfo.enabled) active++;
                    }
                    if (active != 1) throw new AssertionError("Multi-package query straddled an atomic batch");
                }
            } catch (Throwable t) { failure.set(t); }
        });
        reader.start();
        try {
            for (int i = 0; i < 30; i++) {
                h.client.setComponentEnabledSettings(List.of(app(a, (i & 1) == 0 ? 2 : 1),
                        app(b, (i & 1) == 0 ? 1 : 2)));
            }
        } finally { done.set(true); reader.join(10000); }
        check(!reader.isAlive() && failure.get() == null, "Atomic multi-package query failed: " + failure.get());
        h.drain();
    }

    private static void caches() throws Exception {
        String a = "fixture.enabled.cache";
        Harness h = new Harness(record(a, true));
        Field manager = field(Class.forName("android.app.ActivityThread"), "sPackageManager");
        Object previous = manager.get(null); manager.set(null, h.api);
        int flags = PackageManager.GET_ACTIVITIES | PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;
        try {
            // Prime Android's actual PackageInfo cache, then query through the public
            // wrapper after publication. Host uses official process-local test nonces;
            // ART's ServiceFixtureRunner owns the real writable shared-memory region.
            PackageManager.invalidatePackageInfoCache();
            PackageInfo before = h.client.getPackageInfo(a, flags);
            check(before.applicationInfo.enabled && before.activities.length == 1, "Cache setup failed");
            h.client.setApplicationEnabledSetting(a, 3, KEEP);
            PackageInfo after = h.client.getPackageInfo(a, flags);
            check(!after.applicationInfo.enabled && after.activities == null,
                    "Successful publication left cached PackageInfo enabled");
            check(before.applicationInfo.enabled && before.activities.length == 1, "Publication mutated an old client snapshot");
            h.client.setApplicationEnabledSetting(a, 1, KEEP);
            check(h.client.getPackageInfo(a, flags).applicationInfo.enabled, "Re-enable did not invalidate cached disabled metadata");
        } finally { manager.set(null, previous); }
        h.drain();
    }

    private static void validation() throws Exception {
        String a = "fixture.enabled.validation";
        Harness h = new Harness(record(a, true));
        expect(IllegalArgumentException.class, () -> h.client.setComponentEnabledSettings(List.of()), "Empty batch accepted");
        expect(IllegalArgumentException.class, () -> h.client.setComponentEnabledSettings(List.of(
                app(a, 3), part(a, "Missing", 2, KEEP))), "Invalid last entry accepted");
        check(h.client.getApplicationEnabledSetting(a) == 0, "Rejected batch changed its valid first entry");
        expect(IllegalArgumentException.class, () -> h.client.setComponentEnabledSettings(List.of(app(a, 1), app(a, 2))), "Duplicate app accepted");
        expect(IllegalArgumentException.class, () -> h.client.setComponentEnabledSettings(List.of(
                part(a, "Main", 1, KEEP), part(a, "Main", 2, KEEP))), "Duplicate component accepted");
        expect(IllegalArgumentException.class, () -> h.client.setComponentEnabledSettings(List.of(
                part(a, "Main", 2, KEEP), part(a, "Alias", 1, 0))), "Conflicting DONT_KILL flags accepted");
        expect(IllegalArgumentException.class, () -> h.api.setApplicationEnabledSetting(a, 37, KEEP, 0, a), "Invalid enum accepted");
        // The pinned service checks existence before setter input/permissions. These
        // requests must not accidentally target the only installed user, user 0.
        h.api.setApplicationEnabledSetting(a, 2, KEEP, 10, a);
        h.api.setComponentEnabledSetting(component(a, "Main"), 2, KEEP, 10, a);
        h.api.setComponentEnabledSettings(List.of(app(a, 2)), 10, a);
        h.api.setComponentEnabledSettings(null, 10, a);
        h.api.setApplicationEnabledSetting(null, 37, KEEP, -1, a);
        h.api.setComponentEnabledSetting(null, 37, KEEP, -1, a);
        check(h.api.getApplicationEnabledSetting(a, 0) == 0
                && h.api.getComponentEnabledSetting(component(a, "Main"), 0) == 0,
                "Nonexistent-user setter altered user 0");
        check(h.api.getApplicationEnabledSetting(a, 10) == 2
                && h.api.getApplicationEnabledSetting(null, -1) == 2,
                "Nonexistent-user application getter did not report DISABLED");
        check(h.api.getComponentEnabledSetting(component(a, "Main"), 10) == 2
                && h.api.getComponentEnabledSetting(null, 10) == 0,
                "Privileged component lookup lost AOSP user/null ordering");
        expect(IllegalArgumentException.class,
                () -> h.api.getComponentEnabledSetting(component(a, "Main"), -1),
                "Component getter accepted a negative user");
        expect(IllegalArgumentException.class, () -> h.api.getApplicationEnabledSetting("fixture.enabled.unknown", 0), "Unknown package state invented");
        h.client.setComponentEnabledSetting(component(a, "Main"), 3, KEEP);
        check(h.client.getComponentEnabledSetting(component(a, "Main")) == 0, "Application-only enum applied to component");
        if (!art) {
            long previous = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)19001 << 32) | 899913);
                expect(SecurityException.class, () -> h.api.setApplicationEnabledSetting(a, 2, KEEP, 0, a),
                        "Forged package name bypassed permission checking");
                expect(SecurityException.class,
                        () -> h.api.getComponentEnabledSetting(component(a, "Main"), 10),
                        "Component getter skipped cross-user authorization");
                check(h.api.getApplicationEnabledSetting(a, 10) == 2,
                        "Application getter checked permissions before absent user");
                h.api.setComponentEnabledSettings(null, 10, a);
                Binder.restoreCallingIdentity(((long)1019001 << 32) | 899913);
                check(h.api.getComponentEnabledSetting(component(a, "Main"), 10) == 2,
                        "Same-user absent component lookup did not report DISABLED");
                expect(SecurityException.class, () -> h.api.getApplicationEnabledSetting(a, 0),
                        "Application getter allowed another user's identity");
            } finally { Binder.restoreCallingIdentity(previous); }
        }
        h.drain(); check(h.events.isEmpty() && h.kills.isEmpty(), "Rejected/no-op batch caused side effects");
    }

    private static void persistence() throws Exception {
        String a = "fixture.enabled.persistence";
        Harness h = new Harness(record(a, true));
        h.client.setComponentEnabledSettings(List.of(app(a, 3), part(a, "Alias", 1, KEEP)));
        Harness reopened = new Harness(record(a, true));
        check(reopened.client.getApplicationEnabledSetting(a) == 3
                && reopened.client.getComponentEnabledSetting(component(a, "Alias")) == 1, "Restart lost enabled states");
        invoke(reopened.service, "add", new Class<?>[]{ApkPackage.class}, record(a, true));
        check(reopened.client.getApplicationEnabledSetting(a) == 3, "APK update lost installed state");
        // Settle the successful package update's permission notification before
        // testing that the failed enabled-state write queues no further work.
        reopened.drain();
        Path file = Path.of(System.getenv("ANDROID_DATA"), "system/package-enabled-state");
        Path saved = file.resolveSibling("enabled-state.saved"); Files.move(file, saved); Files.createDirectory(file);
        try {
            expect(IllegalStateException.class, () -> reopened.client.setApplicationEnabledSetting(a, 1, KEEP), "Failed persistence returned success");
            check(reopened.client.getApplicationEnabledSetting(a) == 3 && reopened.tasks.isEmpty(),
                    "Failed persistence published memory or side effects");
        } finally { Files.delete(file); Files.move(saved, file); }
        invoke(reopened.service, "remove", new Class<?>[]{String.class}, a);
        invoke(reopened.service, "add", new Class<?>[]{ApkPackage.class}, record(a, true));
        check(reopened.client.getApplicationEnabledSetting(a) == 0
                && reopened.client.getComponentEnabledSetting(component(a, "Alias")) == 0, "Reinstall inherited retired settings");
        Harness afterCrash = new Harness(record(a, true));
        check(afterCrash.client.getApplicationEnabledSetting(a) == 0, "Old disk row revived after uninstall/restart");
        byte[] good = Files.readAllBytes(file); Files.write(file, new byte[]{1, 2, 3});
        try { expect(IllegalStateException.class, () -> new Harness(record(a, true)), "Corrupt store silently re-enabled apps"); }
        finally { Files.write(file, good); }
    }

    private static void quarantine() throws Exception {
        String a = "fixture.enabled.quarantine";
        Harness h = new Harness(record(a, true)); h.client.setApplicationEnabledSetting(a, 3, KEEP);
        Path root = Path.of(System.getenv("ANDROID_DATA")), code = root.resolve("app/" + a + "-0");
        Files.createDirectories(code); Files.writeString(code.resolve("base.apk"), "fixture");
        Class<?> type = Class.forName("com.gesher.server.PackageRemovalStorage");
        var constructor = type.getDeclaredConstructor(String.class, String.class); constructor.setAccessible(true);
        Object removal = constructor.newInstance(a, code.toString());
        invoke(removal, "stage", new Class<?>[0]);
        Path owner = root.resolve("system/package-enabled-owners/" + a);
        check(!Files.exists(owner), "Uninstall quarantine left enabled-state ownership live");
        invoke(removal, "rollback", new Class<?>[0]);
        check(new Harness(record(a, true)).client.getApplicationEnabledSetting(a) == 3, "Removal rollback lost enabled state");
        removal = constructor.newInstance(a, code.toString()); invoke(removal, "stage", new Class<?>[0]);
        var reserves = type.getDeclaredMethod("reserves", Path.class); reserves.setAccessible(true);
        check((boolean) reserves.invoke(null, code), "Pending removal lost its code generation reservation");
        var recover = type.getDeclaredMethod("recover", Map.class); recover.setAccessible(true);
        recover.invoke(null, Map.of(a, code.toString()));
        check(new Harness(record(a, true)).client.getApplicationEnabledSetting(a) == 3,
                "Crash recovery discarded enabled state of a retained installation");
        check(!(boolean) reserves.invoke(null, code), "Recovered removal retained its generation reservation");
        removal = constructor.newInstance(a, code.toString()); invoke(removal, "stage", new Class<?>[0]);
        invoke(removal, "discard", new Class<?>[0]);
        check(new Harness(record(a, true)).client.getApplicationEnabledSetting(a) == 0,
                "Committed quarantine let a later install inherit stale enabled state");
    }

    private static final class RestartTask extends FutureTask<Void> implements ScheduledFuture<Void> {
        RestartTask(Runnable runnable) { super(runnable, null); }
        @Override public long getDelay(TimeUnit unit) { return 0; }
        @Override public int compareTo(Delayed other) { return 0; }
    }

    private static Object processRecord(Object atm, Class<?> processType) throws Exception {
        var constructor = processType.getDeclaredConstructor(atm.getClass(), int.class, IApplicationThread.class);
        constructor.setAccessible(true);
        return constructor.newInstance(atm, 0, new IApplicationThread.Default());
    }

    @SuppressWarnings("unchecked")
    private static void lifecycle() throws Exception {
        String a = "fixture.enabled.lifecycle";
        ApkPackage owner = record(a, true); Harness h = new Harness(owner);
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> processType = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
        var atmCtor = atmType.getDeclaredConstructor(PackageManagerService.class); atmCtor.setAccessible(true);
        Object atm = atmCtor.newInstance(h.service), process = processRecord(atm, processType);
        AtomicInteger exits = new AtomicInteger();
        set(process, "pid", 817251); set(process, "packageName", a);
        set(process, "thread", new IApplicationThread.Default() {
            @Override public void scheduleExit() { check(!Thread.holdsLock(atm), "Exit callback held ATM monitor"); exits.incrementAndGet(); }
        });
        set(atm, "mPm", h.service); set(atm, "mProcesses", new ArrayList<>(List.of(process)));
        set(h.service, "mEnabledStateKiller", (Consumer<ApkPackage>)pkg -> {
            try { invoke(atm, "restartForEnabledState", new Class<?>[]{ApkPackage.class}, pkg); }
            catch (Exception failure) { throw new AssertionError(failure); }
        });
        h.client.setApplicationEnabledSetting(a, 3, KEEP); h.drain();
        check(exits.get() == 0, "DONT_KILL_APP dispatched actual exit");
        h.client.setApplicationEnabledSetting(a, 1, 0); h.drain();
        check(exits.get() == 1 && field(processType, "reaped").getBoolean(process)
                && field(processType, "exiting").getBoolean(process), "flags=0 did not retire the captured process for recovery");

        // Run production sticky-service death/restart policy with only its timer
        // controlled. A disabled service must not be resurrected by queued work.
        Class<?> servicesType = Class.forName("com.gesher.server.ActiveServices");
        var servicesCtor = servicesType.getDeclaredConstructor(atmType, PackageManagerService.class,
                Class.forName("com.gesher.server.NotificationManagerService"));
        servicesCtor.setAccessible(true); Object services = servicesCtor.newInstance(atm, h.service, null);
        List<RestartTask> timers = new ArrayList<>();
        set(services, "mRestarts", Proxy.newProxyInstance(PackageEnabledStateContractTest.class.getClassLoader(),
                new Class<?>[]{ScheduledExecutorService.class}, (proxy, method, args) -> {
                    if (method.getName().equals("schedule") && args[0] instanceof Runnable task) {
                        RestartTask timer = new RestartTask(task); timers.add(timer); return timer;
                    }
                    throw new AssertionError("Unexpected timer operation " + method);
                }));
        Class<?> serviceType = Class.forName("com.gesher.server.ActiveServices$ServiceRecord");
        var serviceCtor = serviceType.getDeclaredConstructor(android.content.pm.ServiceInfo.class, ApkPackage.class);
        serviceCtor.setAccessible(true);
        Object running = serviceCtor.newInstance(owner.serviceInfo(a + ".Service", 0), owner);
        set(running, "app", process); set(running, "startRequested", true);
        Map<Object, Object> live = (Map<Object, Object>)field(servicesType, "mServices").get(services);
        live.put(component(a, "Service"), running);
        invoke(services, "processDied", new Class<?>[]{processType}, process);
        check(live.size() == 1 && timers.size() == 1, "Enabled sticky service lost ordinary recovery");
        h.client.setComponentEnabledSetting(component(a, "Service"), 2, KEEP);
        RestartTask timer = timers.remove(0); timer.run(); timer.get();
        check(field(serviceType, "app").get(running) == null && timers.isEmpty(), "Queued restart resurrected disabled service");
        set(running, "app", process);
        invoke(services, "processDied", new Class<?>[]{processType}, process);
        check(live.isEmpty() && timers.isEmpty(), "Disabled service death scheduled sticky recovery");

        h.client.setApplicationEnabledSetting(a, 2, 0);
        invoke(h.service, "add", new Class<?>[]{ApkPackage.class}, record(a, true));
        set(process, "exiting", false); h.drain();
        check(exits.get() == 1, "Queued old-installation event killed the updated package");
    }

    private static Object hostRetirementAtm;
    private static int hostNativeKills;
    private static boolean hostNativeSuccess;
    private static boolean nativeKillObserved(int pid, long identity) {
        check(Thread.holdsLock(hostRetirementAtm), "Native termination was not serialized with attachment");
        check(pid == 817253 && identity == 0x123456789L, "Native termination lost captured child identity");
        hostNativeKills++; return hostNativeSuccess;
    }

    private static void retirementGeneration() throws Exception {
        String a = "fixture.enabled.retirement";
        ApkPackage owner = record(a, true); Harness h = new Harness(owner);
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> processType = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
        var atmCtor = atmType.getDeclaredConstructor(PackageManagerService.class); atmCtor.setAccessible(true);
        Object atm = atmCtor.newInstance(h.service), first = processRecord(atm, processType);
        Object stale = processRecord(atm, processType), replacement = processRecord(atm, processType);
        // Invalid OS PIDs make this regression safe even if run on Windows with the
        // old implementation. The superseded record's recovery flag still exposes it.
        for (Object process : new Object[]{first, stale, replacement}) set(process, "packageName", a);
        set(first, "pid", -817251); set(stale, "pid", -817252); set(replacement, "pid", -817252);
        set(stale, "spawned", true);
        AtomicInteger staleCallbacks = new AtomicInteger(), replacementCallbacks = new AtomicInteger();
        set(stale, "thread", new IApplicationThread.Default() {
            @Override public void scheduleExit() { staleCallbacks.incrementAndGet(); }
        });
        set(replacement, "thread", new IApplicationThread.Default() {
            @Override public void scheduleExit() { replacementCallbacks.incrementAndGet(); }
        });
        set(first, "thread", new IApplicationThread.Default() {
            @Override public void scheduleExit() {
                check(!Thread.holdsLock(atm), "Retirement callback held ATM monitor");
                try {
                    synchronized (atm) { invoke(atm, "registerAttachedProcess", new Class<?>[]{processType}, replacement); }
                } catch (Exception failure) { throw new AssertionError(failure); }
            }
        });
        set(atm, "mPm", h.service); set(atm, "mProcesses", new ArrayList<>(List.of(first, stale)));
        invoke(atm, "restartForEnabledState", new Class<?>[]{ApkPackage.class}, owner);
        check(field(processType, "pidSuperseded").getBoolean(stale)
                && !field(processType, "reaped").getBoolean(stale)
                && !field(processType, "exiting").getBoolean(replacement)
                && staleCallbacks.get() == 0 && replacementCallbacks.get() == 0,
                "An earlier exit callback let retirement act on a superseded process");

        if (!art) {
            Object child = processRecord(atm, processType);
            set(child, "pid", 817253); set(child, "packageName", a); set(child, "spawned", true);
            set(child, "nativeStartTime", 0x123456789L);
            AtomicInteger exits = new AtomicInteger();
            set(child, "thread", new IApplicationThread.Default() {
                @Override public void scheduleExit() {
                    check(!Thread.holdsLock(atm), "Missing native handle fallback held ATM monitor"); exits.incrementAndGet();
                }
            });
            set(atm, "mProcesses", new ArrayList<>(List.of(child)));
            hostRetirementAtm = atm; hostNativeKills = 0; hostNativeSuccess = true;
            try {
                invoke(atm, "restartForEnabledState", new Class<?>[]{ApkPackage.class}, owner);
                check(hostNativeKills == 1 && exits.get() == 0, "Successful exact-child kill fell back to another dispatch");
                set(child, "exiting", false); hostNativeSuccess = false;
                invoke(atm, "restartForEnabledState", new Class<?>[]{ApkPackage.class}, owner);
                check(hostNativeKills == 2 && exits.get() == 1, "Retired child handle stranded its live Binder capability");
                set(child, "exiting", false); set(child, "nativeStartTime", 0L);
                invoke(atm, "restartForEnabledState", new Class<?>[]{ApkPackage.class}, owner);
                check(hostNativeKills == 2 && exits.get() == 2, "Unknown child identity attempted native termination");
            } finally { hostRetirementAtm = null; }
        }
    }

    private static void standalone() throws Exception {
        String a = "fixture.enabled.standalone";
        Harness device = new Harness(record(a, true));
        device.client.setApplicationEnabledSetting(a, 3, KEEP);
        Path root = Path.of(System.getenv("ANDROID_DATA"));
        Path file = root.resolve("system/package-enabled-state"), owner = root.resolve("system/package-enabled-owners/" + a);
        byte[] before = Files.readAllBytes(file), ownerBefore = Files.readAllBytes(owner);
        Harness local = new Harness(false, record(a, true));
        check(local.client.getApplicationEnabledSetting(a) == 0, "Standalone inherited device policy");
        local.client.setComponentEnabledSettings(List.of(app(a, 1), part(a, "Alias", 1, KEEP)));
        invoke(local.service, "add", new Class<?>[]{ApkPackage.class}, record(a, true));
        check(local.client.getApplicationEnabledSetting(a) == 1
                && local.client.getComponentEnabledSetting(component(a, "Alias")) == 1, "Standalone update lost local state");
        check(Arrays.equals(before, Files.readAllBytes(file)) && Arrays.equals(ownerBefore, Files.readAllBytes(owner))
                && device.client.getApplicationEnabledSetting(a) == 3, "Standalone toggles overwrote device policy or owner");
        invoke(local.service, "remove", new Class<?>[]{String.class}, a);
        invoke(local.service, "add", new Class<?>[]{ApkPackage.class}, record(a, true));
        check(local.client.getApplicationEnabledSetting(a) == 0 && Files.exists(owner), "Standalone uninstall retired device owner");
        local.drain(); device.drain();
    }

    private static void crossProcess(boolean write) throws Exception {
        String a = "fixture.enabled.crossprocess";
        Harness h = new Harness(record(a, true));
        if (write) h.client.setComponentEnabledSettings(List.of(app(a, 3), part(a, "Alias", 1, KEEP)));
        check(h.client.getApplicationEnabledSetting(a) == 3
                && h.client.getComponentEnabledSetting(component(a, "Alias")) == 1,
                "Separate VM did not retain application/component state");
    }

    private static Object readInstallEntry(Method readRegistry, Path registry, String packageName,
            Path codePath, long firstInstall, long lastUpdate) throws Exception {
        Files.writeString(registry, packageName + "\t" + codePath + "\t" + firstInstall
                + "\t" + lastUpdate + "\n");
        return ((Map<?, ?>) readRegistry.invoke(null)).get(packageName);
    }

    /** Real install publication/storage boundary; the APK parser has already supplied the records. */
    private static void installationPublication() throws Exception {
        String existing = "fixture.enabled.install.existing", added = "fixture.enabled.install.added";
        Harness h = new Harness(record(existing, true));
        Path root = Path.of(System.getenv("ANDROID_DATA"));
        Path firstCode = root.resolve("app/" + existing + "-1"), addedCode = root.resolve("app/" + added + "-1");
        Files.createDirectories(firstCode); Files.createDirectories(addedCode);
        Class<?> installerType = Class.forName("com.gesher.server.Installer");
        Class<?> entryType = Class.forName("com.gesher.server.Installer$Entry");
        var readRegistry = installerType.getDeclaredMethod("readRegistry");
        readRegistry.setAccessible(true);
        Path registry = root.resolve("system/packages.list"), marker = root.resolve("system/package-enabled-owners/" + added);
        Files.createDirectories(registry.getParent());
        // Read the supported legacy rows through Installer, independent of its private Entry constructor.
        Object previous = readInstallEntry(readRegistry, registry, existing, firstCode, 1, 1);
        Object addition = readInstallEntry(readRegistry, registry, added, addedCode, 2, 2);
        Object replacement = readInstallEntry(readRegistry, registry, added, addedCode, 2, 3);
        Object failed = readInstallEntry(readRegistry, registry, added, addedCode, 2, 4);
        Map<String, Object> entries = new LinkedHashMap<>(); entries.put(existing, previous);
        var installerCtor = installerType.getDeclaredConstructor(PackageManagerService.class,
                Class.forName("com.gesher.server.ActivityTaskManagerService"), Map.class);
        installerCtor.setAccessible(true);
        Object installer = installerCtor.newInstance(h.service, null, entries);
        invoke(installer, "writeRegistry", new Class<?>[0]);
        byte[] before = Files.readAllBytes(registry);
        ApkPackage candidate = record(added, true);
        Files.createDirectories(marker); // A deterministic owner-publication I/O failure.
        expect(IllegalStateException.class,
                () -> invoke(installer, "publishInstalled", new Class<?>[]{entryType, ApkPackage.class}, addition, candidate),
                "Installer committed a package whose owner could not be established");
        check(Arrays.equals(before, Files.readAllBytes(registry)) && !entries.containsKey(added)
                && h.service.get(added) == null, "Owner failure changed the registry or live package set");
        Files.delete(marker);
        invoke(installer, "publishInstalled", new Class<?>[]{entryType, ApkPackage.class}, addition, candidate);
        check(h.service.get(added) == candidate && entries.get(added) == addition
                && Files.readString(registry).contains(added + "\t"), "Prepared install failed to publish");

        h.client.setApplicationEnabledSetting(added, 3, KEEP);
        byte[] owner = Files.readAllBytes(marker);
        ApkPackage update = record(added, true);
        invoke(installer, "publishInstalled", new Class<?>[]{entryType, ApkPackage.class}, replacement, update);
        check(h.service.get(added) == update && h.client.getApplicationEnabledSetting(added) == 3
                && Arrays.equals(owner, Files.readAllBytes(marker)), "Update replaced its installation owner or enabled state");

        // A later registry failure must not leave Installer's in-memory source of
        // truth pointing at the unpublished update.
        Path saved = registry.resolveSibling("packages.saved");
        Files.move(registry, saved); Files.createDirectory(registry);
        try {
            expect(java.io.IOException.class,
                    () -> invoke(installer, "publishInstalled", new Class<?>[]{entryType, ApkPackage.class}, failed, record(added, true)),
                    "Registry replacement failure returned success");
            check(entries.get(added) == replacement && h.service.get(added) == update
                    && h.client.getApplicationEnabledSetting(added) == 3,
                    "Registry failure published a candidate or retained its pending entry");
        } finally { Files.delete(registry); Files.move(saved, registry); }

        // After preparation, repeat publication must not perform fallible owner I/O.
        ApkPackage prepared = record(added, true);
        invoke(h.service, "prepareAdd", new Class<?>[]{ApkPackage.class}, prepared);
        Path savedOwner = marker.resolveSibling(added + ".saved");
        Files.move(marker, savedOwner); Files.createDirectory(marker);
        try {
            invoke(installer, "publishInstalled", new Class<?>[]{entryType, ApkPackage.class}, replacement, prepared);
            check(h.service.get(added) == prepared && h.client.getApplicationEnabledSetting(added) == 3,
                    "Prepared publication repeated owner I/O after registry commit");
        } finally { Files.delete(marker); Files.move(savedOwner, marker); }
        h.drain();
    }

    public static int run(boolean useBinder, String group) throws Exception {
        art = useBinder; checks = transactions = 0;
        if (group.equals("all") || group.equals("application")) applicationBatch();
        if (group.equals("all") || group.equals("metadata")) metadata();
        if (group.equals("all") || group.equals("batch")) batches();
        if (group.equals("all") || group.equals("batch")) multiplePackages();
        if (group.equals("all") || group.equals("cache")) caches();
        if (group.equals("all") || group.equals("validation")) validation();
        if (group.equals("all") || group.equals("persistence")) persistence();
        if (group.equals("all") || group.equals("quarantine")) quarantine();
        if (group.equals("all") || group.equals("lifecycle")) lifecycle();
        if (group.equals("all") || group.equals("generation")) retirementGeneration();
        if (group.equals("all") || group.equals("standalone")) standalone();
        if (group.equals("all") || group.equals("installation")) installationPublication();
        if (group.equals("write") || group.equals("read")) crossProcess(group.equals("write"));
        if (useBinder) check(transactions > 0, "Generated IPackageManager Binder proxy was not exercised");
        return checks;
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        int count = run(true, args.length == 0 ? "all" : args[0]);
        System.out.println("Package enabled state contracts passed: " + count + " checks; AIDL transactions=" + transactions);
    }
}
