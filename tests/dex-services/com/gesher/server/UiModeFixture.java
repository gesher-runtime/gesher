/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import static android.app.UiModeManager.*;
import static android.content.res.Configuration.*;

import android.app.IApplicationThread;
import android.app.IUiModeManager;
import android.app.IUiModeManagerCallback;
import android.app.servertransaction.ActivityConfigurationChangeItem;
import android.app.servertransaction.ActivityRelaunchItem;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.ConfigurationChangeItem;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.time.Instant;
import java.time.LocalTime;
import java.time.ZoneId;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;
import sun.misc.Unsafe;

/** AOSP-derived assertions on production policy, persistence and ATM transaction selection. */
public final class UiModeFixture {
    private static final String A = "fixture.uimode.a", B = "fixture.uimode.b", C = "fixture.uimode.c";
    private static int checks, nextPid = 819600;
    private static final Unsafe UNSAFE;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe) f.get(null); }
        catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    interface Operation { void run() throws Exception; }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message); checks++;
    }
    private static void rejects(Class<? extends Throwable> type, Operation action, String reason) throws Exception {
        try { action.run(); } catch (Throwable failure) {
            if (type.isInstance(failure)) { checks++; return; }
            throw new AssertionError(reason + ": wrong failure", failure);
        }
        throw new AssertionError(reason + ": accepted");
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name); field.setAccessible(true); return field;
    }
    private static Object get(Object value, String name) throws Exception { return field(value.getClass(), name).get(value); }
    private static void set(Object value, String name, Object data) throws Exception { field(value.getClass(), name).set(value, data); }
    private static Object construct(Class<?> type, Class<?>[] signature, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(signature); constructor.setAccessible(true);
        return constructor.newInstance(values);
    }
    private static void invoke(Object value, String name, Class<?>[] signature, Object... values) throws Exception {
        call(value.getClass(), value, name, signature, values);
    }
    private static Object call(Class<?> owner, Object value, String name, Class<?>[] signature,
            Object... values) throws Exception {
        Method method = owner.getDeclaredMethod(name, signature); method.setAccessible(true);
        try { return method.invoke(value, values); }
        catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Error error) throw error;
            if (failure.getCause() instanceof Exception cause) throw cause;
            throw new AssertionError(failure.getCause());
        }
    }
    private static Class<?> uiType(String name) throws Exception {
        return Class.forName("com.gesher.server.UiModeManagerService$" + name);
    }
    // ART defines production classes on the boot class path, while this fixture
    // comes from services-tests.dex. Reflect on nonpublic test boundaries instead
    // of relying on a matching package name or widening the production API.
    private static UiModeManagerService service(PackageManagerService pm, ActivityTaskManagerService atm,
            Source source, File file, String standalone, Consumer<String> changed) throws Exception {
        Class<?> sourceType = uiType("Source");
        return (UiModeManagerService) construct(UiModeManagerService.class,
                new Class<?>[]{PackageManagerService.class, ActivityTaskManagerService.class, sourceType,
                        File.class, String.class, Consumer.class},
                pm, atm, source.bridge(sourceType), file, standalone, changed);
    }
    private static void refresh(UiModeManagerService service) throws Exception {
        // Match production's single observation worker. An AUTO refresh queued
        // before this call must not publish its old snapshot after this one.
        try {
            ((ScheduledThreadPoolExecutor) get(service, "mWorker")).submit(() -> {
                invoke(service, "refreshEnvironment", new Class<?>[]{});
                return null;
            }).get(3, TimeUnit.SECONDS);
        } catch (ExecutionException failure) {
            if (failure.getCause() instanceof Error error) throw error;
            if (failure.getCause() instanceof Exception cause) throw cause;
            throw new AssertionError(failure.getCause());
        }
    }
    private static boolean between(LocalTime now, LocalTime start, LocalTime end) throws Exception {
        return (Boolean) call(UiModeManagerService.class, null, "between",
                new Class<?>[]{LocalTime.class, LocalTime.class, LocalTime.class}, now, start, end);
    }
    private static String enabledStateOwner(ApkPackage pkg) throws Exception {
        return (String) call(ApkPackage.class, pkg, "enabledStateOwner", new Class<?>[]{});
    }
    private static void onUiModeChanged(String name) {
        try { call(LocaleManagerService.class, null, "onUiModeChanged", new Class<?>[]{String.class}, name); }
        catch (RuntimeException | Error failure) { throw failure; }
        catch (Exception failure) { throw new AssertionError("Cannot dispatch UiMode configuration", failure); }
    }
    private static Class<?> nested(String name) throws Exception {
        return Class.forName("com.gesher.server.ActivityTaskManagerService$" + name);
    }
    private static ApkPackage pkg(String name, int uid) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        for (String suffix : List.of("Handles", "Relaunch")) {
            ParsedActivityImpl activity = new ParsedActivityImpl();
            activity.setName(name + "." + suffix); activity.setPackageName(name);
            activity.setProcessName(name); activity.setEnabled(true); activity.setExported(true);
            activity.setConfigChanges(suffix.equals("Handles") ? ActivityInfo.CONFIG_UI_MODE : 0);
            parsed.addActivity(activity);
        }
        parsed.hideAsFinal();
        ApkPackage result = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        set(result, "packageName", name); set(result, "parsed", parsed); set(result, "apkPath", parsed.getBaseApkPath());
        set(result, "mDataDir", "/fixture/" + name + "/data"); set(result, "mUid", uid);
        set(result, "sharedLibraries", field(Class.forName("com.gesher.server.SharedLibraryGraph"), "EMPTY").get(null));
        return result;
    }
    private record Twilight(boolean night, long nextChange) { }
    private static final class Source {
        volatile long time = Instant.parse("2026-11-17T12:00:00Z").toEpochMilli();
        volatile ZoneId zone = ZoneId.of("UTC");
        volatile int theme = 1, display = WindowsHost.DISPLAY_STATE_ON;
        volatile boolean power, closed;
        volatile Twilight twilight;
        final AtomicInteger events = new AtomicInteger();
        Runnable changed;
        Object read(boolean needsTwilight) throws Exception {
            Twilight observed = needsTwilight ? twilight : null;
            Class<?> twilightType = uiType("Twilight");
            Object value = observed == null ? null : construct(twilightType,
                    new Class<?>[]{boolean.class, long.class}, observed.night(), observed.nextChange());
            return construct(uiType("Environment"),
                    new Class<?>[]{int.class, int.class, Boolean.class, twilightType}, theme, display, power, value);
        }
        Object bridge(Class<?> sourceType) {
            // A nonpublic interface's proxy must be defined by its owning loader.
            return Proxy.newProxyInstance(sourceType.getClassLoader(), new Class<?>[]{sourceType},
                    (proxy, method, args) -> switch (method.getName()) {
                        case "now" -> time;
                        case "zone" -> zone;
                        case "takeEvents" -> events.getAndSet(0);
                        case "read" -> read((Boolean) args[0]);
                        case "listen" -> { changed = (Runnable) args[0]; yield null; }
                        case "close" -> { closed = true; changed = null; yield null; }
                        case "equals" -> proxy == args[0];
                        case "hashCode" -> System.identityHashCode(proxy);
                        case "toString" -> "UiMode fixture source";
                        default -> throw new AssertionError("Unexpected source method: " + method);
                    });
        }
        void at(String time) { this.time = Instant.parse(time).toEpochMilli(); }
    }
    private static final class RecordingThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        final List<Object> items = new ArrayList<>();
        Object uiLock;
        @Override public IBinder asBinder() { return binder; }
        @Override public synchronized void scheduleTransaction(ClientTransaction transaction) {
            if (uiLock != null) check(!Thread.holdsLock(uiLock), "Application transaction under UiMode monitor");
            items.addAll(transaction.getTransactionItems());
        }
        synchronized void clear() { items.clear(); }
        synchronized long count(Class<?> kind) { return items.stream().filter(kind::isInstance).count(); }
    }
    private record App(Object record, int pid, int uid, RecordingThread thread) { }
    private static void as(App app, Operation operation) throws Exception {
        long before = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long) app.uid << 32) | (app.pid & 0xffffffffL)); operation.run(); }
        finally { Binder.restoreCallingIdentity(before); }
    }
    private static IBinder remote(IBinder target) {
        return new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, data, reply, flags);
            }
        };
    }
    private static final class Harness {
        final ApkPackage a = pkg(A, 12345), b = pkg(B, 12345), c = pkg(C, 12346);
        final PackageManagerService pm = (PackageManagerService) construct(PackageManagerService.class,
                new Class<?>[]{java.util.Collection.class, boolean.class}, List.of(a, b, c), false);
        final ActivityTaskManagerService atm = (ActivityTaskManagerService) construct(ActivityTaskManagerService.class,
                new Class<?>[]{PackageManagerService.class}, pm);
        final ActivityManagerService am = (ActivityManagerService) construct(ActivityManagerService.class,
                new Class<?>[]{ActivityTaskManagerService.class, PackageManagerService.class,
                        Class.forName("com.gesher.server.NotificationManagerService")}, atm, pm, null);
        final Source source = new Source();
        final File file;
        final UiModeManagerService service;
        final IUiModeManager api;
        final App appA, appB, appC;
        final List<String> dispatches = new ArrayList<>();
        Harness(boolean binder, File state) throws Exception {
            file = state;
            construct(LocaleManagerService.class,
                    new Class<?>[]{ActivityManagerService.class, ActivityTaskManagerService.class, PackageManagerService.class},
                    am, atm, pm);
            service = service(pm, atm, source, file, null, name -> {
                synchronized (dispatches) { dispatches.add(name); }
                onUiModeChanged(name);
            });
            api = binder ? IUiModeManager.Stub.asInterface(remote(StandIn.of(IUiModeManager.class, service).asBinder())) : service;
            appA = process(a); appB = process(b); appC = process(c);
        }
        @SuppressWarnings("unchecked") App process(ApkPackage pkg) throws Exception {
            RecordingThread thread = new RecordingThread();
            int pid = nextPid++;
            Object record;
            long before = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) pkg.uid() << 32) | pid);
                record = construct(nested("ProcessRecord"), new Class<?>[]{ActivityTaskManagerService.class,
                        int.class, IApplicationThread.class}, atm, pid, thread);
            } finally { Binder.restoreCallingIdentity(before); }
            invoke(atm, "assignPackage", new Class<?>[]{nested("ProcessRecord"), String.class, String.class},
                    record, pkg.packageName, pkg.packageName);
            set(record, "ready", true); set(record, "config", LocaleManagerService.configurationFor(pkg.packageName));
            ((List<Object>) get(atm, "mProcesses")).add(record);
            thread.uiLock = get(service, "mLock");
            return new App(record, pid, pkg.uid(), thread);
        }
        @SuppressWarnings({"unchecked", "rawtypes"}) Object activity(String name, String suffix, App app, String state) throws Exception {
            ActivityInfo info = pm.getActivityInfo(new ComponentName(name, name + "." + suffix), 0, 0);
            Class<?> type = nested("ActivityRecord");
            Object record = construct(type, new Class<?>[]{ActivityTaskManagerService.class, ActivityInfo.class,
                    Intent.class, int.class, int.class, String.class, type, String.class, int.class},
                    atm, info, new Intent().setComponent(new ComponentName(name, info.name)), 1, 0, "android", null, null, -1);
            set(record, "state", Enum.valueOf((Class) nested("State"), state));
            set(record, "config", LocaleManagerService.configurationFor(name));
            ((List<Object>) get(atm, "mActivities")).add(record);
            invoke(atm, "attach", new Class<?>[]{type, nested("ProcessRecord")}, record, app.record);
            return record;
        }
        void refresh() throws Exception { UiModeFixture.refresh(service); }
        int night(String name) { return LocaleManagerService.configurationFor(name).uiMode & UI_MODE_NIGHT_MASK; }
        int global() { return UiModeManagerService.getUiModeNight(); }
        void clear() { appA.thread.clear(); appB.thread.clear(); appC.thread.clear(); dispatches.clear(); }
    }
    private static final class Callback extends IUiModeManagerCallback.Stub {
        int calls;
        @Override public void notifyContrastChanged(float value) { calls++; }
        @Override public void notifyForceInvertStateChanged(int value) { calls++; }
        @Override public void notifyForceInvertOverrideStateChanged() { calls++; }
    }

    private static IUiModeManager api(UiModeManagerService service, boolean binder) {
        return binder ? IUiModeManager.Stub.asInterface(remote(
                StandIn.of(IUiModeManager.class, service).asBinder())) : service;
    }
    private static void awaitWorker(UiModeManagerService service) throws Exception {
        ((ScheduledThreadPoolExecutor) get(service, "mWorker")).submit(() -> {}).get(3, TimeUnit.SECONDS);
    }
    private static Properties saved(File file) throws Exception {
        Properties values = new Properties();
        try (var in = Files.newInputStream(file.toPath())) { values.load(in); }
        return values;
    }
    private static void blockStore(File parent, File backup) throws Exception {
        Files.move(parent.toPath(), backup.toPath());
        Files.writeString(parent.toPath(), "blocked policy directory");
    }
    private static void restoreStore(File parent, File backup) throws Exception {
        Files.delete(parent.toPath());
        Files.move(backup.toPath(), parent.toPath());
    }
    private static void scheduledBoundary(boolean binder, PackageManagerService pm,
            ActivityTaskManagerService atm, String zone, long start, long end,
            String before, String after, int initial, int expected, boolean endBoundary) throws Exception {
        Source source = new Source();
        source.zone = ZoneId.of(zone); source.at(before);
        source.display = WindowsHost.DISPLAY_STATE_OFF;
        CountDownLatch applied = new CountDownLatch(1);
        try (UiModeManagerService service = service(pm, atm, source, null, null, ignored -> {
            if (UiModeManagerService.getUiModeNight() == expected) applied.countDown();
        })) {
            IUiModeManager api = api(service, binder);
            api.setCustomNightModeStart(start); api.setCustomNightModeEnd(end);
            synchronized (get(service, "mLock")) {
                api.setNightMode(MODE_NIGHT_CUSTOM);
                check(UiModeManagerService.getUiModeNight() == initial, "Wrong inclusive boundary state at " + before);
                Method next = UiModeManagerService.class.getDeclaredMethod("nextBoundary", long.class, long.class, boolean.class);
                next.setAccessible(true);
                check((long) next.invoke(service, source.time, endBoundary ? end : start, endBoundary)
                                == Instant.parse(after).toEpochMilli(),
                        "Microsecond boundary rounded to the wrong observable millisecond at " + before);
                check(((ScheduledFuture<?>) get(service, "mAlarm")).getDelay(TimeUnit.MILLISECONDS) <= 20,
                        "Imminent custom boundary postponed until a later refresh at " + before);
                source.at(after);
            }
            check(applied.await(3, TimeUnit.SECONDS) && UiModeManagerService.getUiModeNight() == expected,
                    "Scheduled worker did not publish the boundary transition at " + after);
        }
        check(source.closed && source.changed == null, "Boundary worker retained its source after close");
    }
    private static void transientStorage(boolean binder, PackageManagerService pm,
            ActivityTaskManagerService atm, File directory, int mode, int custom) throws Exception {
        File parent = new File(directory, "transient-" + mode + "-" + custom);
        File backup = new File(directory, parent.getName() + "-saved");
        Files.createDirectory(parent.toPath());
        File file = new File(parent, "policy.properties");
        Source source = new Source(); source.display = WindowsHost.DISPLAY_STATE_OFF;
        source.twilight = new Twilight(false, source.time + 60_000);
        AtomicInteger dispatches = new AtomicInteger();
        try (UiModeManagerService service = service(pm, atm, source, file, null,
                ignored -> dispatches.incrementAndGet())) {
            IUiModeManager api = api(service, binder);
            if (mode == MODE_NIGHT_CUSTOM) api.setNightModeCustomType(custom); else api.setNightMode(mode);
            awaitWorker(service);
            byte[] durable = Files.readAllBytes(file.toPath());
            int initial = mode == MODE_NIGHT_YES ? UI_MODE_NIGHT_YES : UI_MODE_NIGHT_NO;
            int opposite = initial == UI_MODE_NIGHT_YES ? UI_MODE_NIGHT_NO : UI_MODE_NIGHT_YES;
            int overlay = opposite == UI_MODE_NIGHT_YES ? MODE_ATTENTION_THEME_OVERLAY_NIGHT : MODE_ATTENTION_THEME_OVERLAY_DAY;
            blockStore(parent, backup);
            try {
                int calls = dispatches.get();
                api.setAttentionModeThemeOverlay(overlay);
                check(api.getAttentionModeThemeOverlay() == overlay
                                && UiModeManagerService.getUiModeNight() == opposite && dispatches.get() == calls + 1,
                        "Transient attention did not synchronously publish with an unavailable store in mode " + mode);
                if (mode == MODE_NIGHT_NO) {
                    rejects(IllegalStateException.class, () -> api.setNightModeActivated(true), "Failed activation changed durable mode");
                    check(api.getNightMode(0) == MODE_NIGHT_NO && api.getAttentionModeThemeOverlay() == overlay
                                    && UiModeManagerService.getUiModeNight() == opposite,
                            "Failed activation partially cleared attention or changed resources");
                } else if (custom == MODE_NIGHT_CUSTOM_TYPE_BEDTIME) {
                    rejects(IllegalStateException.class,
                            () -> api.setNightModeActivatedForCustomMode(MODE_NIGHT_CUSTOM_TYPE_BEDTIME, true),
                            "Failed matched bedtime activation changed a durable override");
                }
                api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_OFF);
                check(UiModeManagerService.getUiModeNight() == initial,
                        "Attention clearing or a failed bedtime request changed the underlying mode");
                if (mode == MODE_NIGHT_AUTO || mode == MODE_NIGHT_CUSTOM && custom == MODE_NIGHT_CUSTOM_TYPE_SCHEDULE) {
                    source.display = WindowsHost.DISPLAY_STATE_ON;
                    source.at("2026-11-17T23:00:00Z");
                    source.twilight = new Twilight(true, source.time + 60_000);
                    refresh(service);
                    check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                            "Unavailable storage bypassed interactive automatic deferral");
                    source.display = WindowsHost.DISPLAY_STATE_OFF;
                    refresh(service);
                    check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES,
                            "Ordinary automatic night transition depended on policy storage");
                    source.at("2026-11-18T12:00:00Z");
                    source.twilight = new Twilight(false, source.time + 60_000);
                    refresh(service);
                    check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                            "Ordinary automatic day transition depended on policy storage");
                }
            } finally { restoreStore(parent, backup); }
            check(java.util.Arrays.equals(durable, Files.readAllBytes(file.toPath())),
                    "A transient update rewrote unrelated durable policy");
            refresh(service);
            check(api.getNightMode(0) == mode && UiModeManagerService.getUiModeNight() == initial,
                    "Storage recovery committed a previously rejected request");
        }
    }
    private static void derivedOverrideStorage(boolean binder, PackageManagerService pm,
            ActivityTaskManagerService atm, File directory, int mode, boolean attention) throws Exception {
        File parent = new File(directory, "override-" + mode + "-" + attention);
        File backup = new File(directory, parent.getName() + "-saved");
        Files.createDirectory(parent.toPath());
        File file = new File(parent, "policy.properties");
        Source source = new Source(); source.display = WindowsHost.DISPLAY_STATE_OFF;
        source.twilight = new Twilight(false, source.time + 60_000);
        try (UiModeManagerService service = service(pm, atm, source, file, null, ignored -> {})) {
            IUiModeManager api = api(service, binder);
            api.setNightMode(mode); awaitWorker(service);
            check(api.setNightModeActivated(true), "Could not establish a durable automatic override");
            if (attention) api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_DAY);
            blockStore(parent, backup);
            try {
                source.at("2026-11-17T23:00:00Z");
                source.twilight = new Twilight(true, source.time + 60_000);
                refresh(service);
                if (attention) api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_OFF);
                check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES
                                && api.getAttentionModeThemeOverlay() == MODE_ATTENTION_THEME_OVERLAY_OFF,
                        "A derived override reset failed to publish when its cache write failed");
                source.at("2026-11-18T12:00:00Z");
                source.twilight = new Twilight(false, source.time + 60_000);
                refresh(service);
                check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                        "Failed storage retained an obsolete automatic override in memory");
            } finally { restoreStore(parent, backup); }
            refresh(service);
            check(saved(file).getProperty("overrideOn").equals("false")
                            && saved(file).getProperty("overrideOff").equals("false"),
                    "Storage recovery did not save the pending derived override reset");
        }
    }

    private static void invalidCustomTime(boolean binder, PackageManagerService pm,
            ActivityTaskManagerService atm) throws Exception {
        for (boolean start : new boolean[]{true, false}) {
            for (long bad : new long[]{-1, 86_400_000_000L, Long.MAX_VALUE}) {
                Source source = new Source();
                AtomicInteger dispatches = new AtomicInteger();
                try (UiModeManagerService service = service(pm, atm, source, null, null,
                        ignored -> dispatches.incrementAndGet())) {
                    IUiModeManager api = api(service, binder);
                    api.setNightMode(MODE_NIGHT_NO);
                    api.setNightMode(MODE_NIGHT_CUSTOM);
                    awaitWorker(service);
                    check((Boolean) get(get(service, "mState"), "waitForInactive"),
                            "CUSTOM did not establish the inactive-device wait");
                    long beforeStart = api.getCustomNightModeStart();
                    long beforeEnd = api.getCustomNightModeEnd();
                    Object before = get(service, "mState");
                    int published = dispatches.get();
                    if (start) api.setCustomNightModeStart(bad); else api.setCustomNightModeEnd(bad);
                    Object after = get(service, "mState");
                    check(api.getCustomNightModeStart() == beforeStart && api.getCustomNightModeEnd() == beforeEnd,
                            "Invalid custom endpoint changed a configured time");
                    check(!(Boolean) get(after, "waitForInactive"),
                            "Invalid custom endpoint retained the inactive-device wait");
                    check(get(before, "overrideOn").equals(get(after, "overrideOn"))
                                    && get(before, "overrideOff").equals(get(after, "overrideOff"))
                                    && dispatches.get() == published
                                    && UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                            "Invalid custom endpoint changed overrides or published resources");
                    api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_NIGHT);
                    check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES
                                    && dispatches.get() == published + 1,
                            "Attention stayed deferred after invalid custom time cleared the wait");
                }
            }
        }
    }

    private static void attentionWait(boolean binder, PackageManagerService pm,
            ActivityTaskManagerService atm, int mode) throws Exception {
        Source source = new Source();
        source.twilight = new Twilight(false, source.time + 60_000);
        AtomicInteger dispatches = new AtomicInteger();
        try (UiModeManagerService service = service(pm, atm, source, null, null,
                ignored -> dispatches.incrementAndGet())) {
            IUiModeManager api = api(service, binder);
            api.setNightMode(MODE_NIGHT_NO); api.setNightMode(mode); awaitWorker(service);
            int calls = dispatches.get();
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_NIGHT);
            check(api.getAttentionModeThemeOverlay() == MODE_ATTENTION_THEME_OVERLAY_NIGHT
                            && UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO && dispatches.get() == calls,
                    "Attention bypassed an automatic selection's pending inactive-device wait");
            source.display = WindowsHost.DISPLAY_STATE_DIMMED; refresh(service);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO, "Dimmed display released an attention wait");
            source.display = WindowsHost.DISPLAY_STATE_UNKNOWN; refresh(service);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO, "Unknown display released an attention wait");
            source.display = WindowsHost.DISPLAY_STATE_ON;
            source.power = true; refresh(service);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES, "Battery saver did not bypass pending publication");
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_DAY);
            source.power = false; refresh(service);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES,
                    "Battery-saver publication incorrectly cleared the existing wait");
            source.display = WindowsHost.DISPLAY_STATE_OFF; refresh(service);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO, "Display-off did not release pending attention");
            source.display = WindowsHost.DISPLAY_STATE_ON; refresh(service);
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_NIGHT);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES,
                    "Interactive attention was deferred with no pending wait");
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_OFF);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                    "Interactive attention clearing was deferred after the wait ended");

            api.setNightMode(MODE_NIGHT_NO); api.setNightMode(mode); awaitWorker(service);
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_NIGHT);
            check(api.setNightModeActivated(false), "Explicit activation failed to clear the pending wait");
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_NIGHT);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES,
                    "Explicit activation retained an obsolete attention wait");
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_OFF);
            source.at("2026-11-17T23:00:00Z");
            source.twilight = new Twilight(true, source.time + 60_000);
            refresh(service);
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_NIGHT);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                    "An observed automatic transition did not rearm the inactive-device wait");
            source.display = WindowsHost.DISPLAY_STATE_OFF; refresh(service);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES,
                    "Display-off did not publish the automatic transition's pending overlay");
            source.display = WindowsHost.DISPLAY_STATE_ON; refresh(service);
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_DAY);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                    "An unchanged display-on observation recreated a released wait");
            if (mode == MODE_NIGHT_CUSTOM) {
                source.events.set(WindowsHost.EVENT_TIME_CHANGED); refresh(service);
                api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_NIGHT);
                check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                        "A real clock-change event did not retain custom-mode deferral");
                source.display = WindowsHost.DISPLAY_STATE_OFF; refresh(service);
                check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES,
                        "Clock-change wait did not release at display-off");
                source.display = WindowsHost.DISPLAY_STATE_ON; refresh(service);
            }
            api.setNightMode(MODE_NIGHT_NO); api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_OFF);
            api.setNightMode(mode); awaitWorker(service);
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_NIGHT);
            api.setNightMode(MODE_NIGHT_YES);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES,
                    "Explicit YES policy did not release the previous automatic wait");
            api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_DAY);
            check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                    "Explicit mode selection left a stale attention wait");
        }
    }

    private static void observe(Source source, boolean night, int day) {
        source.at("2026-11-" + day + (night ? "T23:00:00Z" : "T12:00:00Z"));
        source.twilight = new Twilight(night, source.time + 86_400_000);
    }
    private static void deferredOverride(boolean binder, PackageManagerService pm,
            ActivityTaskManagerService atm, File directory, int mode, boolean active, String update) throws Exception {
        String description = "mode=" + mode + ", manual=" + active + ", update=" + update;
        File file = new File(directory, "deferred-" + mode + "-" + active + "-" + update + ".properties");
        Source source = new Source(); source.display = WindowsHost.DISPLAY_STATE_OFF;
        observe(source, !active, 17);
        int manual = active ? UI_MODE_NIGHT_YES : UI_MODE_NIGHT_NO;
        int expected = update.equals("none") || update.equals("steady-power") ? manual
                : active ? UI_MODE_NIGHT_NO : UI_MODE_NIGHT_YES;
        try (UiModeManagerService service = service(pm, atm, source, file, null, ignored -> {})) {
            IUiModeManager api = api(service, binder);
            api.setNightMode(mode); awaitWorker(service);
            check(api.setNightModeActivated(active) && UiModeManagerService.getUiModeNight() == manual,
                    "Manual override setup failed: " + description);
            byte[] selected = Files.readAllBytes(file.toPath());
            if (update.equals("steady-power")) {
                source.power = true; refresh(service);
            }
            source.display = WindowsHost.DISPLAY_STATE_ON;
            observe(source, active, 18); refresh(service);
            check(java.util.Arrays.equals(selected, Files.readAllBytes(file.toPath())),
                    "Deferred catch-up retired the saved override: " + description);
            check((boolean) get(get(service, "mState"), "computed") == active,
                    "Deferred catch-up rewrote computed policy: " + description);
            if (update.equals("attention")) {
                int overlay = active ? MODE_ATTENTION_THEME_OVERLAY_DAY : MODE_ATTENTION_THEME_OVERLAY_NIGHT;
                api.setAttentionModeThemeOverlay(overlay);
                check(api.getAttentionModeThemeOverlay() == overlay
                                && UiModeManagerService.getUiModeNight() == manual,
                        "Attention did not preserve the pending resource wait: " + description);
                check((boolean) get(get(service, "mState"), "computed") != active,
                        "Attention failed to recompute policy while a wait was pending: " + description);
                api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_OFF);
            } else if (update.equals("power-change")) {
                source.power = true; refresh(service);
                source.power = false; refresh(service);
            }
            observe(source, !active, 19); refresh(service);
            if (update.equals("none") || update.equals("steady-power")) {
                check(java.util.Arrays.equals(selected, Files.readAllBytes(file.toPath()))
                                && (boolean) get(get(service, "mState"), "computed") == active,
                        "Deferred return observation changed the override or computed cache: " + description);
            }
            if (update.equals("steady-power")) {
                source.power = false; refresh(service);
                check(UiModeManagerService.getUiModeNight() == manual,
                        "Automatic callbacks recomputed merely because power saver was already active: " + description);
            }
            source.display = WindowsHost.DISPLAY_STATE_OFF; refresh(service);
            check(UiModeManagerService.getUiModeNight() == expected,
                    "Inactivity evaluated the wrong retained override: " + description);
        }
        if (mode == MODE_NIGHT_AUTO) {
            check(saved(file).getProperty("computed").equals(Boolean.toString(expected == UI_MODE_NIGHT_YES)),
                    "Shutdown saved an incorrectly recomputed deferred AUTO cache: " + description);
        }
    }

    /** Host child process leaves this service live so the production shutdown hook must save it. */
    public static void prepareShutdown(File file) throws Exception {
        Source source = new Source(); source.display = WindowsHost.DISPLAY_STATE_OFF;
        source.twilight = new Twilight(false, source.time + 60_000);
        PackageManagerService pm = (PackageManagerService) construct(PackageManagerService.class,
                new Class<?>[]{java.util.Collection.class, boolean.class}, List.of(), false);
        ActivityTaskManagerService atm = (ActivityTaskManagerService) construct(ActivityTaskManagerService.class,
                new Class<?>[]{PackageManagerService.class}, pm);
        UiModeManagerService service = service(pm, atm,
                source, file, null, ignored -> {});
        service.setNightMode(MODE_NIGHT_AUTO); awaitWorker(service);
        byte[] durable = Files.readAllBytes(file.toPath());
        source.twilight = new Twilight(true, source.time + 60_000);
        refresh(service);
        check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES
                        && java.util.Arrays.equals(durable, Files.readAllBytes(file.toPath())),
                "AUTO publication wrote its shutdown cache eagerly");
    }

    public static int run(boolean binder) throws Exception {
        checks = 0;
        File directory = Files.createTempDirectory("gesher-uimode-").toFile();
        File state = new File(directory, "policy.properties");
        Harness h = new Harness(binder, state);
        try {
            check(h.api.getCurrentModeType(0) == UI_MODE_TYPE_NORMAL && h.api.isUiModeLocked(), "Unsupported car/dock capability advertised");
            check(!h.api.isNightModeLocked(), "Normal Android night mode unexpectedly locked");
            check(h.global() == UI_MODE_NIGHT_NO && h.api.getNightMode(0) == MODE_NIGHT_NO, "Observed light preference");
            h.source.theme = 0; h.refresh();
            check(h.global() == UI_MODE_NIGHT_YES, "Observed Windows dark preference was not applied");
            h.source.theme = -1; h.refresh();
            check(h.global() == UI_MODE_NIGHT_YES, "Unknown Windows theme fabricated light mode");
            h.api.setNightMode(MODE_NIGHT_NO); h.source.theme = 0; h.refresh();
            check(h.global() == UI_MODE_NIGHT_NO, "Host preference overwrote explicit Android selection");
            rejects(IllegalArgumentException.class, () -> h.api.setNightMode(99), "Invalid global mode");
            rejects(IllegalArgumentException.class, () -> h.api.setApplicationNightMode(-99), "Invalid application mode");
            as(h.appA, () -> {
                h.api.setNightMode(MODE_NIGHT_YES);
                check(h.global() == UI_MODE_NIGHT_YES, "Unlocked Android setNightMode permission changed");
                rejects(SecurityException.class, () -> h.api.setNightModeCustomType(MODE_NIGHT_CUSTOM_TYPE_BEDTIME), "Custom type permission");
                rejects(SecurityException.class, () -> h.api.getNightModeCustomType(), "Custom type getter permission");
                rejects(SecurityException.class, () -> h.api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_NIGHT), "Attention overlay permission");
                check(!h.api.setNightModeActivated(false), "Unprivileged activation succeeded");
                rejects(SecurityException.class, () -> h.api.getContrast(1), "Cross-user contrast permission");
                rejects(SecurityException.class, () -> h.api.getForceInvertOverrideState(0, C), "Foreign UID inversion query");
                check(h.api.getForceInvertOverrideState(0, B) == FORCE_INVERT_PACKAGE_ALLOWED, "Shared UID package query rejected");
            });
            check(h.global() == UI_MODE_NIGHT_YES, "Denied activation changed state");
            as(new App(null, 919899, 19999, null), () ->
                    check(!h.api.setNightModeActivated(false), "Unknown application activation did not fail with false"));
            Callback callback = new Callback();
            h.api.addCallback(callback, 0); h.api.addCallback(callback, 0);
            check(((android.os.RemoteCallbackList<?>) get(h.service, "mCallbacks")).getRegisteredCallbackCount() == 1, "Duplicate callback registered");
            h.api.setNightMode(MODE_NIGHT_NO); h.api.setNightMode(MODE_NIGHT_YES);
            check(callback.calls == 0, "Night mode emitted contrast/inversion callbacks");
            check(h.api.getForceInvertState(0) == FORCE_INVERT_TYPE_OFF && h.api.getContrast(0) == 0f, "Unimplemented accessibility capability reported");
            as(new App(null, android.os.Process.myPid(), android.os.Process.SYSTEM_UID, null), () ->
                    check(h.api.getForceInvertOverrideState(0, "android") == FORCE_INVERT_PACKAGE_ALLOWED, "Platform package ownership query failed"));
            h.api.removeCallback(callback, 0);
            check(((android.os.RemoteCallbackList<?>) get(h.service, "mCallbacks")).getRegisteredCallbackCount() == 0, "Callback removal retained registration");

            h.api.setNightMode(MODE_NIGHT_NO);
            Object handles = h.activity(A, "Handles", h.appA, "RESUMED");
            Object relaunches = h.activity(A, "Relaunch", h.appA, "RESUMED");
            Object background = h.activity(B, "Relaunch", h.appB, "STOPPED");
            h.clear(); int sequence = LocaleManagerService.configurationFor(A).seq;
            as(h.appA, () -> h.api.setApplicationNightMode(MODE_NIGHT_YES));
            check(h.global() == UI_MODE_NIGHT_NO && h.night(A) == UI_MODE_NIGHT_YES, "Application override changed device mode");
            check(h.night(B) == UI_MODE_NIGHT_NO && h.night(C) == UI_MODE_NIGHT_NO, "Application override escaped its package/shared UID");
            check(LocaleManagerService.configurationFor(A).seq > sequence, "Night configuration sequence did not advance");
            check(h.appA.thread.count(ConfigurationChangeItem.class) == 1, "Application process missed configuration transaction");
            check(h.appA.thread.count(ActivityConfigurationChangeItem.class) == 1, "configChanges=uiMode activity missed callback transaction");
            check(h.appA.thread.count(ActivityRelaunchItem.class) == 1, "Unhandled activity missed state-preserving relaunch");
            check(h.appB.thread.items.isEmpty() && h.appC.thread.items.isEmpty(), "Unrelated application received configuration transaction");
            check(h.dispatches.equals(List.of(A)), "Application mode dispatched a global change");
            as(h.appA, () -> h.api.setApplicationNightMode(MODE_NIGHT_AUTO));
            check(h.night(A) == h.global(), "Application AUTO did not clear override");
            as(h.appA, () -> h.api.setApplicationNightMode(MODE_NIGHT_YES));
            as(h.appA, () -> h.api.setApplicationNightMode(MODE_NIGHT_CUSTOM));
            check(h.night(A) == h.global(), "Application CUSTOM did not clear override");
            // A foreign-package activity can live in the same process. Its override must
            // travel in the activity transaction even when that process's mode is unchanged.
            Object sharedActivity = h.activity(B, "Handles", h.appA, "RESUMED");
            h.clear();
            as(h.appB, () -> h.api.setApplicationNightMode(MODE_NIGHT_YES));
            check(h.appA.thread.count(ConfigurationChangeItem.class) == 0, "Shared process inherited another package's global mode");
            check(h.appA.thread.count(ActivityConfigurationChangeItem.class) == 1, "Shared-process activity missed per-package override");
            check((Boolean) get(background, "configPending"), "Stopped activity was not marked for later configuration");
            check(h.appB.thread.count(ActivityRelaunchItem.class) == 0, "Stopped activity was prematurely relaunched");
            for (Object item : h.appA.thread.items) if (item instanceof ActivityConfigurationChangeItem) {
                Configuration override = (Configuration) get(item, "mConfiguration");
                check((override.uiMode & UI_MODE_NIGHT_MASK) == UI_MODE_NIGHT_YES, "Activity transaction omitted its package override");
            }
            App forged = new App(h.appA.record, h.appA.pid, h.appC.uid, h.appA.thread);
            as(forged, () -> rejects(SecurityException.class, () -> h.api.setApplicationNightMode(MODE_NIGHT_YES), "PID with mismatched UID"));
            as(h.appB, () -> h.api.setApplicationNightMode(MODE_NIGHT_AUTO));

            h.api.setNightMode(MODE_NIGHT_NO);
            h.source.twilight = new Twilight(true, h.source.time + 10_000);
            h.api.setNightMode(MODE_NIGHT_AUTO); h.refresh();
            check(h.api.getNightMode(0) == MODE_NIGHT_AUTO, "AUTO getter reported effective mode");
            check(h.global() == UI_MODE_NIGHT_NO, "Astronomical automatic transition ignored interactive deferral");
            h.source.display = WindowsHost.DISPLAY_STATE_UNKNOWN; h.refresh();
            check(h.global() == UI_MODE_NIGHT_NO, "Unknown display claimed inactive state");
            h.source.display = WindowsHost.DISPLAY_STATE_DIMMED; h.refresh();
            check(h.global() == UI_MODE_NIGHT_NO, "Dimmed display incorrectly treated as inactive");
            h.source.display = WindowsHost.DISPLAY_STATE_OFF; h.refresh();
            check(h.global() == UI_MODE_NIGHT_YES, "Display-off did not apply pending astronomical night");
            h.source.theme = 1; h.refresh();
            check(h.global() == UI_MODE_NIGHT_YES, "AUTO followed Windows light preference instead of twilight");
            h.source.twilight = null; h.refresh();
            check(h.global() == UI_MODE_NIGHT_YES, "Missing actual location fabricated a twilight state");
            h.source.twilight = new Twilight(false, h.source.time + 20_000); h.refresh();
            check(h.global() == UI_MODE_NIGHT_NO, "Astronomical day did not apply while inactive");
            check(h.api.setNightModeActivated(true) && h.global() == UI_MODE_NIGHT_YES, "Authorized AUTO manual activation");
            h.refresh(); check(h.global() == UI_MODE_NIGHT_YES, "Manual AUTO override did not persist until the underlying transition");
            h.source.twilight = new Twilight(true, h.source.time + 30_000); h.refresh();
            h.source.twilight = new Twilight(false, h.source.time + 40_000); h.refresh();
            check(h.global() == UI_MODE_NIGHT_NO, "Manual override was not reset when twilight caught up");

            h.api.setNightMode(MODE_NIGHT_CUSTOM);
            check(h.api.getNightModeCustomType() == MODE_NIGHT_CUSTOM_TYPE_SCHEDULE, "CUSTOM default subtype");
            check(h.api.getCustomNightModeStart() == 79_200_000_000L && h.api.getCustomNightModeEnd() == 21_600_000_000L, "Schedule units/default times");
            h.api.setCustomNightModeStart(80_000_000_001L);
            check(h.api.getCustomNightModeStart() == 80_000_000_001L, "Microsecond custom precision lost");
            h.api.setCustomNightModeStart(-1); h.api.setCustomNightModeEnd(Long.MAX_VALUE);
            check(h.api.getCustomNightModeStart() == 80_000_000_001L && h.api.getCustomNightModeEnd() == 21_600_000_000L, "Invalid custom time changed settings");
            h.api.setCustomNightModeStart(79_200_000_000L);
            h.source.at("2026-11-17T23:00:00Z"); h.refresh();
            check(h.global() == UI_MODE_NIGHT_YES, "Overnight custom schedule did not turn on");
            h.source.at("2026-11-18T07:00:00Z"); h.refresh();
            check(h.global() == UI_MODE_NIGHT_NO, "Overnight custom schedule did not turn off");
            h.source.zone = ZoneId.of("Asia/Tokyo"); h.source.at("2026-11-18T14:00:00Z"); h.refresh();
            check(h.global() == UI_MODE_NIGHT_YES, "Schedule ignored real local timezone");
            check(between(LocalTime.of(6, 0), LocalTime.of(22, 0), LocalTime.of(6, 0)), "Android inclusive end boundary changed");
            check(between(LocalTime.NOON, LocalTime.of(6, 0), LocalTime.of(6, 0)), "Equal Android schedule endpoints must cover all day");
            rejects(IllegalArgumentException.class, () -> h.api.setNightModeCustomType(44), "Invalid custom subtype");
            check(!h.api.setNightModeActivatedForCustomMode(MODE_NIGHT_CUSTOM_TYPE_BEDTIME, true), "Mismatched custom activation reported success");
            h.source.display = WindowsHost.DISPLAY_STATE_ON;
            h.api.setNightModeCustomType(MODE_NIGHT_CUSTOM_TYPE_BEDTIME);
            check(h.global() == UI_MODE_NIGHT_YES, "Remembered bedtime activation lost");
            check(h.api.setNightModeActivatedForCustomMode(MODE_NIGHT_CUSTOM_TYPE_BEDTIME, false) && h.global() == UI_MODE_NIGHT_NO, "Bedtime explicit transition deferred");
            h.api.setAttentionModeThemeOverlay(MODE_ATTENTION_THEME_OVERLAY_NIGHT);
            check(h.global() == UI_MODE_NIGHT_YES, "Attention overlay not applied");
            h.api.setNightModeActivated(false);
            check(h.api.getAttentionModeThemeOverlay() == MODE_ATTENTION_THEME_OVERLAY_OFF && h.global() == UI_MODE_NIGHT_NO, "User activation did not clear attention overlay");
            h.api.setNightMode(MODE_NIGHT_NO); h.source.power = true; h.refresh();
            check(h.global() == UI_MODE_NIGHT_YES && h.api.getNightMode(0) == MODE_NIGHT_NO, "Real power saver must override resources without changing selected mode");
            h.source.power = false; h.refresh(); check(h.global() == UI_MODE_NIGHT_NO, "Leaving power saver did not restore selected mode");

            h.api.setNightMode(MODE_NIGHT_YES);
            as(h.appA, () -> h.api.setApplicationNightMode(MODE_NIGHT_NO));
            check(state.isFile(), "Settings never persisted");
            h.service.close(); check(h.source.closed, "Service close retained host listener");
            Source restartedSource = new Source();
            try (UiModeManagerService restart = service(h.pm, h.atm, restartedSource, state, null, ignored -> {})) {
                check(restart.getNightMode(0) == MODE_NIGHT_YES && h.night(A) == UI_MODE_NIGHT_NO, "Global/application settings lost across service restart");
                String owner = enabledStateOwner(h.a);
                set(h.a, "mEnabledOwner", java.util.UUID.randomUUID().toString());
                check(h.night(A) == UI_MODE_NIGHT_YES, "A later installation inherited stale application preference");
                set(h.a, "mEnabledOwner", owner);
            }
            check(restartedSource.closed, "Restart source listener retained");
            check(!((List<?>) get(h.pm, "mPackageRemoval")).contains(get(h.service, "mRemovalListener")), "Closed UI service retained package-removal listener");
            File blocker = new File(directory, "not-a-directory");
            Files.writeString(blocker.toPath(), "fixture");
            try (UiModeManagerService failed = service(h.pm, h.atm, new Source(),
                    new File(blocker, "settings"), null, ignored -> {})) {
                rejects(IllegalStateException.class, () -> failed.setNightMode(MODE_NIGHT_YES), "Failed persistence acknowledged a mode change");
                check(failed.getNightMode(0) == MODE_NIGHT_NO && UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                        "Failed persistence published partially changed state");
            }
            // Isolated app processes have their own persistent file and data lifetime;
            // they do not acquire system permission merely by using a local Binder call.
            File standaloneState = new File(directory, "standalone.properties");
            App local = new App(null, android.os.Process.myPid(), h.a.uid(), null);
            try (UiModeManagerService standalone = service(h.pm, h.atm, new Source(),
                    standaloneState, A, ignored -> {})) {
                as(local, () -> {
                    standalone.setApplicationNightMode(MODE_NIGHT_YES);
                    check(!standalone.setNightModeActivated(true), "Standalone application gained MODIFY_DAY_NIGHT_MODE");
                    rejects(SecurityException.class, () -> standalone.getNightModeCustomType(), "Standalone privileged getter");
                });
                check(h.night(A) == UI_MODE_NIGHT_YES && h.global() == UI_MODE_NIGHT_NO,
                        "Standalone application override affected global policy");
            }
            String beforeOwner = enabledStateOwner(h.a);
            set(h.a, "mEnabledOwner", java.util.UUID.randomUUID().toString());
            try (UiModeManagerService standalone = service(h.pm, h.atm, new Source(),
                    standaloneState, A, ignored -> {})) {
                check(h.night(A) == UI_MODE_NIGHT_YES, "Standalone preferences were lost on a new process PM instance");
            }
            set(h.a, "mEnabledOwner", beforeOwner);
            File autoState = new File(directory, "auto-restart.properties");
            Source knownNight = new Source();
            knownNight.display = WindowsHost.DISPLAY_STATE_OFF;
            knownNight.twilight = new Twilight(true, knownNight.time + 60_000);
            try (UiModeManagerService automatic = service(h.pm, h.atm, knownNight,
                    autoState, null, ignored -> {})) {
                automatic.setNightMode(MODE_NIGHT_AUTO);
                refresh(automatic);
                check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES,
                        "AUTO fixture did not establish a real twilight result");
            }
            Source noLocation = new Source();
            noLocation.theme = 1;
            try (UiModeManagerService automatic = service(h.pm, h.atm, noLocation,
                    autoState, null, ignored -> {})) {
                check(automatic.getNightMode(0) == MODE_NIGHT_AUTO
                                && UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES,
                        "AUTO restart without location lost its last computed night state");
                noLocation.display = WindowsHost.DISPLAY_STATE_OFF;
                noLocation.twilight = new Twilight(false, noLocation.time + 60_000);
                refresh(automatic);
                check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                        "Restored AUTO state did not resume real twilight transitions");
            }
            try (UiModeManagerService automatic = service(h.pm, h.atm, new Source(),
                    autoState, null, ignored -> {})) {
                check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_NO,
                        "AUTO shutdown cache did not retain the last computed transition");
            }

            long start = 79_200_000_000L, end = 21_600_000_000L;
            scheduledBoundary(binder, h.pm, h.atm, "UTC", start, end,
                    "2026-11-18T06:00:00Z", "2026-11-18T06:00:00.001Z", UI_MODE_NIGHT_YES, UI_MODE_NIGHT_NO, true);
            scheduledBoundary(binder, h.pm, h.atm, "UTC", start, end + 1,
                    "2026-11-18T06:00:00Z", "2026-11-18T06:00:00.001Z", UI_MODE_NIGHT_YES, UI_MODE_NIGHT_NO, true);
            scheduledBoundary(binder, h.pm, h.atm, "UTC", start + 1, end,
                    "2026-11-18T22:00:00Z", "2026-11-18T22:00:00.001Z", UI_MODE_NIGHT_NO, UI_MODE_NIGHT_YES, false);
            scheduledBoundary(binder, h.pm, h.atm, "UTC", start, end,
                    "2026-11-18T21:59:59.999Z", "2026-11-18T22:00:00Z", UI_MODE_NIGHT_NO, UI_MODE_NIGHT_YES, false);
            scheduledBoundary(binder, h.pm, h.atm, "UTC", start, 86_399_999_999L,
                    "2026-11-18T23:59:59.999Z", "2026-11-19T00:00:00Z", UI_MODE_NIGHT_YES, UI_MODE_NIGHT_NO, true);
            scheduledBoundary(binder, h.pm, h.atm, "Asia/Kathmandu", start, end,
                    "2026-11-18T00:15:00Z", "2026-11-18T00:15:00.001Z", UI_MODE_NIGHT_YES, UI_MODE_NIGHT_NO, true);
            Source allDay = new Source(); allDay.at("2026-11-18T06:00:00Z");
            allDay.display = WindowsHost.DISPLAY_STATE_OFF;
            try (UiModeManagerService service = service(h.pm, h.atm, allDay, null, null, ignored -> {})) {
                service.setCustomNightModeStart(end); service.setCustomNightModeEnd(end);
                service.setNightMode(MODE_NIGHT_CUSTOM);
                check(UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES
                                && ((ScheduledFuture<?>) get(service, "mAlarm")).getDelay(TimeUnit.MILLISECONDS) > 3_590_000,
                        "Equal inclusive endpoints should remain all day without repeated boundary alarms");
            }
            transientStorage(binder, h.pm, h.atm, directory, MODE_NIGHT_NO, MODE_NIGHT_CUSTOM_TYPE_UNKNOWN);
            transientStorage(binder, h.pm, h.atm, directory, MODE_NIGHT_YES, MODE_NIGHT_CUSTOM_TYPE_UNKNOWN);
            transientStorage(binder, h.pm, h.atm, directory, MODE_NIGHT_AUTO, MODE_NIGHT_CUSTOM_TYPE_UNKNOWN);
            transientStorage(binder, h.pm, h.atm, directory, MODE_NIGHT_CUSTOM, MODE_NIGHT_CUSTOM_TYPE_SCHEDULE);
            transientStorage(binder, h.pm, h.atm, directory, MODE_NIGHT_CUSTOM, MODE_NIGHT_CUSTOM_TYPE_BEDTIME);
            derivedOverrideStorage(binder, h.pm, h.atm, directory, MODE_NIGHT_AUTO, false);
            derivedOverrideStorage(binder, h.pm, h.atm, directory, MODE_NIGHT_CUSTOM, false);
            derivedOverrideStorage(binder, h.pm, h.atm, directory, MODE_NIGHT_AUTO, true);
            invalidCustomTime(binder, h.pm, h.atm);
            attentionWait(binder, h.pm, h.atm, MODE_NIGHT_AUTO);
            attentionWait(binder, h.pm, h.atm, MODE_NIGHT_CUSTOM);
            for (int mode : new int[]{MODE_NIGHT_AUTO, MODE_NIGHT_CUSTOM}) {
                for (boolean active : new boolean[]{false, true}) {
                    for (String update : List.of("none", "attention", "power-change", "steady-power")) {
                        deferredOverride(binder, h.pm, h.atm, directory, mode, active, update);
                    }
                }
            }

            Source replacementNight = new Source(); replacementNight.display = WindowsHost.DISPLAY_STATE_OFF;
            replacementNight.twilight = new Twilight(false, replacementNight.time + 60_000);
            File replacementState = new File(directory, "replacement-auto.properties");
            UiModeManagerService prior = service(h.pm, h.atm, replacementNight,
                    replacementState, null, ignored -> {});
            prior.setNightMode(MODE_NIGHT_AUTO); awaitWorker(prior);
            replacementNight.twilight = new Twilight(true, replacementNight.time + 60_000);
            refresh(prior);
            try (UiModeManagerService replacement = service(h.pm, h.atm, new Source(),
                    replacementState, null, ignored -> {})) {
                check(replacementNight.closed && UiModeManagerService.getUiModeNight() == UI_MODE_NIGHT_YES,
                        "Replacement read AUTO's cache before the previous service saved it");
            }
            File closeParent = new File(directory, "close-failure"); Files.createDirectory(closeParent.toPath());
            File closeBackup = new File(directory, "close-failure-saved");
            File closeState = new File(closeParent, "policy.properties");
            Source closeSource = new Source(); closeSource.display = WindowsHost.DISPLAY_STATE_OFF;
            closeSource.twilight = new Twilight(false, closeSource.time + 60_000);
            try (UiModeManagerService service = service(h.pm, h.atm, closeSource, closeState, null, ignored -> {})) {
                service.setNightMode(MODE_NIGHT_AUTO); awaitWorker(service);
                closeSource.twilight = new Twilight(true, closeSource.time + 60_000);
                refresh(service); service.addCallback(new Callback(), 0);
                blockStore(closeParent, closeBackup);
                try {
                    service.close();
                    check(closeSource.closed && closeSource.changed == null
                                    && ((ScheduledThreadPoolExecutor) get(service, "mWorker")).isShutdown()
                                    && ((android.os.RemoteCallbackList<?>) get(service, "mCallbacks")).getRegisteredCallbackCount() == 0,
                            "Failed shutdown cache write prevented resource cleanup");
                } finally { restoreStore(closeParent, closeBackup); }
                check(saved(closeState).getProperty("computed").equals("false"),
                        "Failed shutdown cache write fabricated a durable computed result");
            }
            Source replacedSource = new Source();
            UiModeManagerService replaced = service(h.pm, h.atm, replacedSource,
                    null, null, ignored -> {});
            replaced.addCallback(new Callback(), 0);
            try (UiModeManagerService replacement = service(h.pm, h.atm, new Source(),
                    null, null, ignored -> {})) {
                check(replacedSource.closed, "Replacing the service retained its old host subscription");
                check(((java.util.concurrent.ScheduledThreadPoolExecutor) get(replaced, "mWorker")).isShutdown(),
                        "Replacing the service retained its old worker");
                check(((android.os.RemoteCallbackList<?>) get(replaced, "mCallbacks")).getRegisteredCallbackCount() == 0,
                        "Replacing the service retained callback registrations");
            }
        } finally {
            h.service.close();
            try (var paths = Files.walk(directory.toPath())) {
                for (var path : paths.sorted(java.util.Comparator.reverseOrder()).toList()) Files.deleteIfExists(path);
            }
        }
        return checks;
    }
}
