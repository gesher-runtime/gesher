/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Activity;
import android.app.ContentProviderHolder;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.DestroyActivityItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.IContentProvider;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.android.internal.pm.pkg.component.ParsedIntentInfoImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import java.lang.reflect.Field;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import sun.misc.Unsafe;

/** Unchanged service/API surface: also compiles against the pre-fix PR head. */
public final class PackageEnabledLifecycleFixture {
    private static final String OWNER = "fixture.enabled.lifecycle.owner";
    private static final String CLIENT = "fixture.enabled.lifecycle.client";
    private static final int UID = 12345;
    private static final int KEEP = PackageManager.DONT_KILL_APP;
    private static final Unsafe UNSAFE;
    private static int checks, nextPid = 817400;
    private static boolean binder;
    private static volatile int fakeChild;
    private static volatile boolean childKilled;
    private static long expectedKillTime = 7001;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Operation { void run() throws Exception; }
    private static synchronized void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason); checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }
    private static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Object rethrow(Throwable failure) throws Exception {
        if (failure instanceof Error e) throw e;
        if (failure instanceof Exception e) throw e;
        throw new AssertionError(failure);
    }
    private static Object construct(Class<?> type, Class<?>[] signature, Object... arguments) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(signature); constructor.setAccessible(true);
        try { return constructor.newInstance(arguments); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static Object invoke(Object target, String name, Class<?>[] signature, Object... arguments) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name, signature); method.setAccessible(true);
        try { return method.invoke(target, arguments); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static final class TestProcess {
        final Object record;
        final int pid, uid;
        final IApplicationThread thread;
        TestProcess(Object record) throws Exception {
            this.record = record; pid = (Integer)get(record, "pid"); uid = (Integer)get(record, "uid");
            thread = (IApplicationThread)get(record, "thread");
        }
        boolean exiting() throws Exception { return (Boolean)get(record, "exiting"); }
    }
    private static ComponentName part(String suffix) { return new ComponentName(OWNER, OWNER + "." + suffix); }
    private static Intent intent(String suffix) { return new Intent().setComponent(part(suffix)); }
    private static void as(TestProcess app, Operation operation) throws Exception {
        long old = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)app.uid << 32) | (app.pid & 0xffffffffL)); operation.run(); }
        finally { Binder.restoreCallingIdentity(old); }
    }
    private static IBinder remote(IBinder target) {
        return new Binder() {
            @Override public IInterface queryLocalInterface(String descriptor) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException { return target.transact(code, data, reply, flags); }
        };
    }
    private static ApkPackage code(String name) throws Exception {
        return code(name, false);
    }
    private static ApkPackage code(String name, boolean collidingAliases) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        for (String suffix : List.of("A", "B")) {
            ParsedServiceImpl service = new ParsedServiceImpl();
            service.setName(name + ".Service" + suffix); service.setPackageName(name);
            service.setEnabled(true); service.setExported(true); service.setProcessName(name);
            parsed.addService(service);
            ParsedActivityImpl activity = new ParsedActivityImpl();
            activity.setName(name + ".Activity" + suffix); activity.setPackageName(name);
            activity.setEnabled(true); activity.setExported(true); activity.setProcessName(name);
            parsed.addActivity(activity);
            ParsedActivityImpl receiver = new ParsedActivityImpl();
            receiver.setName(name + ".Receiver" + suffix); receiver.setPackageName(name);
            receiver.setEnabled(true); receiver.setExported(true); receiver.setProcessName(name);
            ParsedIntentInfoImpl filter = new ParsedIntentInfoImpl();
            filter.getIntentFilter().addAction(name + ".RECEIVE");
            filter.getIntentFilter().setPriority(suffix.equals("A") ? 10 : 0);
            receiver.addIntent(filter); parsed.addReceiver(receiver);
            ParsedProviderImpl provider = new ParsedProviderImpl();
            provider.setName(name + ".Provider" + suffix); provider.setPackageName(name);
            provider.setEnabled(true); provider.setExported(true); provider.setProcessName(name);
            provider.setAuthority(name + ".provider" + suffix); parsed.addProvider(provider);
        }
        if (collidingAliases) for (String suffix : List.of("ServiceA", "ReceiverA", "ProviderA")) {
            ParsedActivityImpl alias = new ParsedActivityImpl();
            alias.setName(name + "." + suffix); alias.setPackageName(name); alias.setEnabled(false);
            alias.setTargetActivity(name + ".ActivityB"); alias.setProcessName(name); parsed.addActivity(alias);
        }
        parsed.hideAsFinal();
        ApkPackage result = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(result, "packageName", name); set(result, "parsed", parsed); set(result, "apkPath", parsed.getBaseApkPath());
        set(result, "mDataDir", "/fixture/" + name + "/data"); set(result, "mUid", UID);
        set(result, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        return result;
    }
    private static final class RecordingThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        final Harness h;
        final List<String> creates = new ArrayList<>(), starts = new ArrayList<>(), stops = new ArrayList<>();
        final List<String> receivers = new ArrayList<>();
        final List<Integer> receiverResults = new ArrayList<>();
        final List<IBinder> providerDeaths = new ArrayList<>();
        final List<IBinder> destroys = new ArrayList<>();
        final List<String> installs = new ArrayList<>(), serviceOrder = new ArrayList<>();
        final Map<IBinder, String> services = new java.util.HashMap<>();
        int exits, binds, registered;
        Operation onCreate, onBind, onExit, onInstall;
        RecordingThread(Harness h) { this.h = h; }
        @Override public IBinder asBinder() { return binder; }
        void invoke(Operation operation) {
            h.unlocked();
            if (operation != null) try { operation.run(); }
            catch (Exception failure) { throw new AssertionError(failure); }
        }
        @Override public void scheduleCreateService(IBinder token, ServiceInfo info,
                CompatibilityInfo compatibility, int state) {
            services.put(token, info.name); creates.add(info.name); serviceOrder.add("create"); invoke(onCreate);
            serviceOrder.add("created");
        }
        @Override public void scheduleServiceArgs(IBinder token, ParceledListSlice args) {
            h.unlocked(); starts.add(services.get(token));
        }
        @Override public void scheduleStopService(IBinder token) {
            h.unlocked(); stops.add(services.get(token)); serviceOrder.add("stop");
        }
        @Override public void scheduleUnbindService(IBinder token, IBinder binding, Intent intent) {
            h.unlocked(); serviceOrder.add("unbind");
        }
        @Override public void scheduleInstallProvider(ProviderInfo info) { installs.add(info.name); invoke(onInstall); }
        @Override public void scheduleBindService(IBinder token, IBinder binding, Intent intent,
                boolean rebind, int state, long sequence) { binds++; invoke(onBind); }
        @Override public void scheduleExit() { exits++; invoke(onExit); }
        @Override public void unstableProviderDied(IBinder provider) { h.unlocked(); providerDeaths.add(provider); }
        @Override public void scheduleReceiver(Intent intent, ActivityInfo info, CompatibilityInfo compat,
                int code, String data, Bundle extras, boolean ordered, boolean delivered, int user,
                int state, int uid, String sender) {
            h.unlocked(); receivers.add(info.name); receiverResults.add(code);
        }
        @Override public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent,
                int code, String data, Bundle extras, boolean ordered, boolean sticky,
                boolean delivered, int user, int state, int uid, String sender) {
            h.unlocked(); registered++;
        }
        @Override public void scheduleTransaction(ClientTransaction transaction) {
            h.unlocked();
            for (var item : transaction.getTransactionItems()) {
                if (item instanceof DestroyActivityItem destroy) destroys.add(destroy.getActivityToken());
            }
        }
    }
    private static final class Connection extends IServiceConnection.Stub {
        final Harness h;
        int disconnected, died;
        Connection(Harness h) { this.h = h; }
        @Override public void connected(ComponentName name, IBinder service, android.app.IBinderSession session, boolean dead) {
            h.unlocked(); if (service == null) disconnected++; if (dead) died++;
        }
    }
    private static final class Harness {
        final ApkPackage owner = code(OWNER), client = code(CLIENT);
        final PackageManagerService pm = (PackageManagerService)construct(PackageManagerService.class,
                new Class<?>[]{java.util.Collection.class, boolean.class}, List.of(owner, client), false);
        final ActivityTaskManagerService atm = (ActivityTaskManagerService)construct(ActivityTaskManagerService.class,
                new Class<?>[]{PackageManagerService.class}, pm);
        final ActivityManagerService am = (ActivityManagerService)construct(ActivityManagerService.class,
                new Class<?>[]{ActivityTaskManagerService.class, PackageManagerService.class,
                        type("NotificationManagerService")}, atm, pm, null);
        final IPackageManager api;
        final IActivityManager amApi;
        final Object services = get(am, "mServices"), broadcasts = get(am, "mBroadcasts"), providers = get(am, "mProviders");
        final ArrayDeque<Runnable> events = new ArrayDeque<>();
        final List<Intent> notifications = new ArrayList<>();
        final List<Object> processes;
        final List<?> pending;
        Harness() throws Exception {
            set(pm, "mPackageEventExecutor", (Executor)events::add);
            set(pm, "mPackageEventSender", (Consumer<Intent>)event -> {
                unlocked(); notifications.add(event); broadcast(event, false, null, 0);
            });
            api = binder ? IPackageManager.Stub.asInterface(remote(StandIn.of(IPackageManager.class, pm).asBinder())) : pm;
            amApi = binder ? IActivityManager.Stub.asInterface(remote(StandIn.of(IActivityManager.class, am).asBinder())) : am;
            processes = (List<Object>)get(atm, "mProcesses");
            pending = (List<?>)get(atm, "mPendingWork");
        }
        void unlocked() {
            check(!Thread.holdsLock(pm) && !Thread.holdsLock(atm) && !Thread.holdsLock(services)
                    && !Thread.holdsLock(broadcasts) && !Thread.holdsLock(providers), "Application callback ran under a lifecycle monitor");
        }
        TestProcess process(String name, boolean ready) throws Exception {
            RecordingThread thread = new RecordingThread(this);
            long old = Binder.clearCallingIdentity();
            Object app;
            try {
                Binder.restoreCallingIdentity(((long)UID << 32) | nextPid);
                app = construct(type("ActivityTaskManagerService$ProcessRecord"),
                        new Class<?>[]{ActivityTaskManagerService.class, int.class, IApplicationThread.class},
                        atm, nextPid++, thread);
            } finally { Binder.restoreCallingIdentity(old); }
            synchronized (atm) {
                invoke(atm, "assignPackage", new Class<?>[]{type("ActivityTaskManagerService$ProcessRecord"),
                        String.class, String.class}, app, name, name);
                set(app, "ready", ready); processes.add(app);
            }
            try {
                invoke(providers, "providersFor", new Class<?>[]{type("ActivityTaskManagerService$ProcessRecord"),
                        android.content.pm.ApplicationInfo.class}, app, pm.getApplicationInfo(name, 0, 0));
            } catch (NoSuchMethodException oldHead) {
                invoke(providers, "providersFor", new Class<?>[]{android.content.pm.ApplicationInfo.class},
                        pm.getApplicationInfo(name, 0, 0));
            }
            return new TestProcess(app);
        }
        @SuppressWarnings({"unchecked", "rawtypes"})
        Object activity(String suffix, int task, TestProcess app, Object result, int request, String state) throws Exception {
            Class<?> recordType = type("ActivityTaskManagerService$ActivityRecord");
            Object record = construct(recordType, new Class<?>[]{ActivityTaskManagerService.class,
                    ActivityInfo.class, Intent.class, int.class, int.class, String.class,
                    recordType, String.class, int.class}, atm, pm.getActivityInfo(part(suffix), 0, 0),
                    intent(suffix), task, 0, "android", result, "result", request);
            set(record, "state", Enum.valueOf((Class)type("ActivityTaskManagerService$State"), state));
            ((List<Object>)get(atm, "mActivities")).add(record);
            if (app != null) invoke(atm, "attach", new Class<?>[]{recordType,
                    type("ActivityTaskManagerService$ProcessRecord")}, record, app.record);
            return record;
        }
        RecordingThread thread(TestProcess app) { return (RecordingThread)app.thread; }
        void start(String suffix) {
            check(part(suffix).equals(am.startService(null, intent(suffix), null, false, "android", null, 0)), "Service failed to start");
        }
        void component(String suffix, int value, int flags) throws Exception {
            api.setComponentEnabledSetting(part(suffix), value, flags, 0, "android");
        }
        void application(int value, int flags) throws Exception {
            api.setApplicationEnabledSetting(OWNER, value, flags, 0, "android");
        }
        void drain() { while (!events.isEmpty()) events.remove().run(); }
        void ready(TestProcess app) throws Exception {
            as(app, () -> am.finishAttachApplication(0, 0));
        }
        Map<?, ?> services() throws Exception { return (Map<?, ?>)get(services, "mServices"); }
        void broadcast(Intent intent, boolean ordered, IIntentReceiver result, int code) {
            am.broadcastIntent(null, intent, null, result, code, null, null, null,
                    android.app.AppOpsManager.OP_NONE, null, ordered, false, 0);
        }
        void finishReceiver(TestProcess app, int code, boolean abort) throws Exception {
            as(app, () -> am.finishReceiver(app.thread.asBinder(), code, null, null, abort, 0));
        }
    }

    private static void services() throws Exception {
        Harness h = new Harness(); var app = h.process(OWNER, true); RecordingThread t = h.thread(app);
        h.start("ServiceA"); h.start("ServiceB");
        Connection connection = new Connection(h);
        check(h.am.bindService(null, null, intent("ServiceA"), null, connection, Context.BIND_AUTO_CREATE,
                "android", 0) == 1, "Live service binding was not accepted");
        h.component("ServiceA", 2, KEEP); h.drain();
        check(t.stops.equals(List.of(OWNER + ".ServiceA")), "DONT_KILL_APP left the disabled running service alive");
        check(!h.services().containsKey(part("ServiceA")) && h.services().containsKey(part("ServiceB")),
                "Component cleanup removed a peer or retained the disabled service");
        check(connection.disconnected == 1 && connection.died == 1,
                "Disabled service did not retire its auto-create binding as binding death");
        check(t.serviceOrder.indexOf("unbind") >= 0 && t.serviceOrder.indexOf("unbind") < t.serviceOrder.indexOf("stop"),
                "Disabled bound service omitted onUnbind or delivered it after onDestroy");
        check(((Map<?, ?>)get(h.services, "mConnections")).isEmpty(),
                "Disabled binding retained an empty client-capability entry");
        check(t.exits == 0 && !app.exiting(), "DONT_KILL_APP retired the shared process");
        h.component("ServiceA", 1, KEEP); h.drain(); h.start("ServiceA");
        check(t.creates.size() == 3 && t.starts.size() == 3, "Explicit re-enable/start reused the retired service generation");

        Harness last = new Harness(); var lastApp = last.process(OWNER, true);
        last.start("ServiceA"); last.component("ServiceA", 2, KEEP); last.drain();
        check(last.thread(lastApp).stops.size() == 1 && last.thread(lastApp).exits == 0 && !lastApp.exiting(),
                "Stopping the last disabled service violated DONT_KILL_APP");

        Harness reentrant = new Harness(); var reentrantApp = reentrant.process(OWNER, true);
        reentrant.thread(reentrantApp).onCreate = () -> reentrant.component("ServiceA", 2, KEEP);
        reentrant.start("ServiceA");
        check(reentrant.thread(reentrantApp).starts.isEmpty(), "A disable during onCreate delivered queued start args");
        check(reentrant.thread(reentrantApp).stops.size() == 1 && reentrant.services().isEmpty(),
                "A disable before event dispatch did not retire a partially created service");
        reentrant.drain();

        Harness binding = new Harness(); var bindingApp = binding.process(OWNER, true);
        Connection bound = new Connection(binding);
        binding.thread(bindingApp).onBind = () -> { binding.component("ServiceA", 2, KEEP); binding.drain(); };
        check(binding.am.bindService(null, null, intent("ServiceA"), null, bound, Context.BIND_AUTO_CREATE,
                "android", 0) == 1, "Reentrant binding failed");
        check(bound.disconnected == 1 && bound.died == 1 && binding.thread(bindingApp).stops.size() == 1
                && binding.thread(bindingApp).exits == 0 && binding.services().isEmpty(),
                "Cleanup during binding dispatch lost final stop or killed the shared process");

        Harness creating = new Harness(); var creatingApp = creating.process(OWNER, true);
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        creating.thread(creatingApp).onCreate = () -> {
            entered.countDown(); check(release.await(3, TimeUnit.SECONDS), "Service creation was not released");
        };
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread launch = new Thread(() -> {
            try { creating.start("ServiceA"); } catch (Throwable problem) { failure.set(problem); }
        }, "enabled-service-create");
        launch.setDaemon(true); launch.start(); check(entered.await(3, TimeUnit.SECONDS), "Service creation did not enter");
        creating.component("ServiceA", 2, KEEP); creating.drain();
        check(creating.thread(creatingApp).stops.isEmpty(), "Cleanup overtook an admitted create callback");
        release.countDown(); launch.join(3000);
        check(!launch.isAlive() && failure.get() == null
                && creating.thread(creatingApp).serviceOrder.equals(List.of("create", "created", "stop"))
                && creating.thread(creatingApp).starts.isEmpty(),
                "Concurrent service cleanup lost lifecycle order or delivered disabled start args: " + failure.get());

        Harness replaced = new Harness(); var replacedApp = replaced.process(OWNER, true);
        replaced.thread(replacedApp).onCreate = () -> {
            replaced.thread(replacedApp).onCreate = null;
            replaced.component("ServiceA", 2, KEEP); replaced.drain();
            replaced.component("ServiceA", 1, KEEP); replaced.drain(); replaced.start("ServiceA");
        };
        replaced.start("ServiceA");
        check(replaced.services().size() == 1 && replaced.thread(replacedApp).creates.size() == 2
                && replaced.thread(replacedApp).starts.size() == 1 && replaced.thread(replacedApp).stops.size() == 1,
                "Old service cleanup removed or started the replacement record");

        Harness families = new Harness(); invoke(families.pm, "add", new Class<?>[]{ApkPackage.class}, code(OWNER, true));
        var familyApp = families.process(OWNER, true);
        check(families.pm.getActivityInfo(part("ServiceA"), 0, 0) == null
                && families.pm.getServiceInfo(part("ServiceA"), 0, 0) != null,
                "Cross-family alias metadata fixture has no independent manifest defaults");
        families.start("ServiceA"); families.broadcast(intent("ReceiverA"), false, null, 0);
        ContentProviderHolder provider = holder(families, "A");
        as(familyApp, () -> families.am.publishContentProviders(familyApp.thread, List.of(provider)));
        check(families.thread(familyApp).creates.size() == 1 && families.thread(familyApp).receivers.size() == 1
                && ((Map<?, ?>)get(families.providers, "mPublished")).containsKey(OWNER + ".providerA"),
                "Disabled activity-alias default shadowed an enabled service, receiver or provider of the same name");
    }

    private static void pending() throws Exception {
        for (boolean eventFirst : List.of(false, true)) for (int flags : new int[]{KEEP, 0}) {
            Harness h = new Harness(); var app = h.process(OWNER, false);
            h.start("ServiceA"); check(h.pending.size() == 1, "Queued start never reached production withProcess");
            h.application(2, flags);
            if (eventFirst) h.drain();
            h.ready(app);
            check(h.thread(app).creates.isEmpty() && h.thread(app).starts.isEmpty(),
                    "Whole-app disable dispatched service work through finishAttachApplication; eventFirst=" + eventFirst);
            check(h.services().isEmpty(), "Rejected attach work retained its service reservation until the event");
            h.drain();
            check(h.pending.isEmpty() && h.services().isEmpty(), "Disabled package retained pending service admission");
            check(h.thread(app).exits == (flags == 0 ? 1 : 0), "Queued disable ignored process-restart flags");
        }
        for (boolean eventFirst : List.of(false, true)) {
            Harness h = new Harness(); var app = h.process(OWNER, false);
            h.start("ServiceA"); h.start("ServiceB"); h.component("ServiceA", 2, KEEP);
            if (eventFirst) h.drain();
            h.ready(app); h.drain();
            check(h.thread(app).creates.equals(List.of(OWNER + ".ServiceB"))
                    && h.thread(app).starts.equals(List.of(OWNER + ".ServiceB")),
                    "Component-only disable dispatched its work or canceled its pending peer");
            check(h.pending.isEmpty() && h.services().size() == 1 && !app.exiting(),
                    "Pending component cleanup did not converge without killing its peer");
        }
        Harness reenabled = new Harness(); var reenabledApp = reenabled.process(OWNER, false);
        reenabled.start("ServiceA"); reenabled.component("ServiceA", 2, KEEP); reenabled.ready(reenabledApp);
        check(reenabled.services().isEmpty(), "Attach cancellation kept old start authority available for a later enable");
        reenabled.component("ServiceA", 1, KEEP); reenabled.drain();
        check(reenabled.thread(reenabledApp).creates.isEmpty(), "Re-enable resurrected canceled pre-attach work");
        reenabled.start("ServiceA");
        check(reenabled.thread(reenabledApp).creates.size() == 1 && reenabled.thread(reenabledApp).starts.size() == 1,
                "Explicit start after re-enable did not create a fresh service");
    }

    private static void events() throws Exception {
        Harness h = new Harness(); var app = h.process(OWNER, true);
        h.start("ServiceA"); h.start("ServiceB");
        h.component("ServiceA", 2, KEEP); h.component("ServiceB", 2, KEEP); h.drain();
        check(h.thread(app).stops.size() == 2 && h.services().isEmpty(), "Coalescing B discarded still-disabled A cleanup");
        check(h.thread(app).exits == 0, "Two KEEP changes retired their process");

        Harness enabled = new Harness(); var valid = enabled.process(OWNER, true);
        enabled.start("ServiceA"); enabled.component("ServiceA", 2, 0); enabled.component("ServiceA", 1, KEEP);
        var admittedAfterRestart = enabled.process(OWNER, true);
        enabled.drain();
        check(enabled.thread(valid).stops.isEmpty() && enabled.thread(valid).exits == 1,
                "A later KEEP enable canceled an already committed process restart");
        check(enabled.thread(admittedAfterRestart).exits == 0,
                "Old restart retired a newly admitted process generation");

        Harness coalesced = new Harness(); var current = coalesced.process(OWNER, false);
        coalesced.start("ServiceA"); coalesced.component("ServiceA", 2, KEEP);
        coalesced.component("ServiceA", 1, KEEP); coalesced.drain(); coalesced.ready(current);
        check(coalesced.thread(current).creates.equals(List.of(OWNER + ".ServiceA"))
                && coalesced.thread(current).starts.size() == 1 && coalesced.thread(current).exits == 0,
                "Coalesced KEEP enable invented cancellation before cleanup or dispatch");

        Harness mixed = new Harness(); var mixedApp = mixed.process(OWNER, true);
        mixed.start("ServiceA"); mixed.start("ServiceB"); mixed.component("ServiceA", 2, 0);
        mixed.component("ServiceB", 2, KEEP); mixed.drain();
        check(mixed.thread(mixedApp).exits == 1 && mixed.services().isEmpty(),
                "Unrelated KEEP change canceled a still-applicable non-KEEP restart");

        Harness batch = new Harness(); var batchApp = batch.process(OWNER, true);
        batch.start("ServiceA"); batch.start("ServiceB");
        batch.api.setComponentEnabledSettings(List.of(
                new PackageManager.ComponentEnabledSetting(part("ServiceA"), 2, 0),
                new PackageManager.ComponentEnabledSetting(part("ServiceB"), 2, 0)), 0, "android");
        batch.component("ServiceA", 1, KEEP); batch.drain();
        check(batch.thread(batchApp).stops.equals(List.of(OWNER + ".ServiceB"))
                && batch.thread(batchApp).exits == 1,
                "Re-enabling one batch entry canceled another entry's required process restart");

        Harness unrelated = new Harness(); var owner = unrelated.process(OWNER, true);
        unrelated.start("ServiceA"); unrelated.component("ServiceA", 2, 0);
        unrelated.api.setApplicationEnabledSetting(CLIENT, 2, KEEP, 0, "android"); unrelated.drain();
        check(unrelated.thread(owner).stops.size() == 1 && unrelated.thread(owner).exits == 1,
                "Another package's snapshot suppressed owner cleanup");

        Harness replacement = new Harness(); replacement.component("ServiceA", 2, 0);
        invoke(replacement.pm, "add", new Class<?>[]{ApkPackage.class}, code(OWNER)); var newer = replacement.process(OWNER, true); replacement.start("ServiceB");
        replacement.drain();
        check(replacement.thread(newer).exits == 0 && replacement.thread(newer).stops.isEmpty(),
                "Old installation event retired replacement work");

        Harness spoof = new Harness(); var victim = spoof.process(OWNER, true); var attacker = spoof.process(CLIENT, true);
        spoof.start("ServiceA"); spoof.component("ServiceA", 2, KEEP);
        Intent fake = new Intent(Intent.ACTION_PACKAGE_CHANGED, android.net.Uri.parse("package:" + OWNER))
                .putExtra(Intent.EXTRA_UID, UID).putExtra(Intent.EXTRA_CHANGED_COMPONENT_NAME_LIST,
                        new String[]{OWNER + ".ServiceA"}).putExtra(Intent.EXTRA_DONT_KILL_APP, false);
        try { as(attacker, () -> spoof.broadcast(fake, false, null, 0)); }
        catch (SecurityException expected) { /* Protected broadcast rejection is also correct. */ }
        spoof.broadcast(fake, false, null, 0); // Even notification from the server is not a lifecycle command.
        check(spoof.thread(victim).stops.isEmpty() && spoof.thread(victim).exits == 0,
                "Untrusted PACKAGE_CHANGED extras acquired lifecycle authority");
        spoof.drain(); check(spoof.thread(victim).stops.size() == 1, "Trusted PM change did not perform cleanup");
    }

    private static void broadcasts() throws Exception {
        for (boolean eventFirst : List.of(false, true)) {
            Harness h = new Harness(); var app = h.process(OWNER, false); h.start("ServiceB");
            h.broadcast(intent("ReceiverA"), false, null, 0); h.broadcast(intent("ReceiverB"), false, null, 0);
            h.component("ReceiverA", 2, KEEP); if (eventFirst) h.drain();
            h.ready(app); h.drain();
            check(h.thread(app).receivers.equals(List.of(OWNER + ".ReceiverB")),
                    "Queued legacy receiver ignored disabled state or canceled its peer");
            h.finishReceiver(app, 0, false);
            check(((List<?>)get(h.broadcasts, "mManifest")).isEmpty() && h.pending.isEmpty(),
                    "Disabled manifest receiver left a pending reservation");
        }
        Harness legacy = new Harness(); var app = legacy.process(OWNER, true); legacy.start("ServiceB");
        legacy.broadcast(intent("ReceiverA"), false, null, 0); legacy.broadcast(intent("ReceiverB"), false, null, 0);
        legacy.component("ReceiverA", 2, KEEP); legacy.drain();
        check(legacy.thread(app).receivers.size() == 1, "Cleanup reused an in-flight receiver acknowledgement slot");
        legacy.finishReceiver(app, 0, false);
        check(legacy.thread(app).receivers.equals(List.of(OWNER + ".ReceiverA", OWNER + ".ReceiverB")),
                "The first receiver's finish did not release exactly its own slot");
        legacy.finishReceiver(app, 0, false);
        check(legacy.thread(app).exits == 0, "Receiver cleanup killed an unrelated service process");

        Harness ordered = new Harness(); var orderedApp = ordered.process(OWNER, true); ordered.start("ServiceB");
        List<Integer> result = new ArrayList<>();
        IIntentReceiver callback = new IIntentReceiver.Stub() {
            @Override public void performReceive(Intent intent, int code, String data, Bundle extras,
                    boolean orderedFlag, boolean sticky, int user) { ordered.unlocked(); result.add(code); }
        };
        ordered.broadcast(new Intent(OWNER + ".RECEIVE").setPackage(OWNER), true, callback, 5);
        ordered.component("ReceiverA", 2, KEEP); ordered.drain();
        check(ordered.thread(orderedApp).receivers.size() == 1 && result.isEmpty(),
                "Disabled active ordered receiver lost acknowledgement ownership");
        ordered.finishReceiver(orderedApp, 99, true);
        check(ordered.thread(orderedApp).receiverResults.equals(List.of(5)) && result.equals(List.of(99)),
                "Cleanup discarded the active receiver's valid ordered result or abort");

        Harness continued = new Harness(); var continuedApp = continued.process(OWNER, true); continued.start("ServiceB");
        continued.broadcast(new Intent(OWNER + ".RECEIVE").setPackage(OWNER), true, null, 5);
        continued.component("ReceiverA", 2, KEEP); continued.drain();
        continued.finishReceiver(continuedApp, 17, false);
        check(continued.thread(continuedApp).receiverResults.equals(List.of(5, 17)),
                "Enabled next receiver lost the completed receiver's ordered result");
        continued.finishReceiver(continuedApp, 0, false);
        registeredReceivers();
    }

    private static void registeredReceivers() throws Exception {
        Harness h = new Harness(); var app = h.process(OWNER, true); h.start("ServiceB");
        String action = OWNER + ".DYNAMIC";
        for (int priority : new int[]{10, 0}) {
            IntentFilter filter = new IntentFilter(action); filter.setPriority(priority);
            IIntentReceiver receiver = new IIntentReceiver.Stub() {
                @Override public void performReceive(Intent i, int c, String d, Bundle e, boolean o, boolean s, int u) { }
            };
            as(app, () -> h.am.registerReceiver(app.thread, OWNER, receiver, filter, null, 0, Context.RECEIVER_EXPORTED));
        }
        h.broadcast(new Intent(action), true, null, 0);
        h.component("ReceiverA", 2, KEEP); h.drain();
        h.finishReceiver(app, 1, false);
        check(h.thread(app).registered == 2, "Component cleanup canceled a pending registered receiver");
        h.finishReceiver(app, 2, false);
        h.broadcast(new Intent(action), true, null, 0);
        h.application(2, KEEP); h.drain();
        check(h.thread(app).registered == 3 && ((Map<?, ?>)get(h.broadcasts, "mRegistered")).size() == 2,
                "Whole-package cleanup dispatched pending dynamic work or removed registrations");
        // AOSP only removes the already pending broadcast. Registration itself
        // continues to belong to the retained process, including new sends.
        h.broadcast(new Intent(action), true, null, 0);
        h.finishReceiver(app, 3, false);
        check(h.thread(app).registered == 4, "Whole-package cleanup retained old pending work or filtered a new registered send");
        h.finishReceiver(app, 4, false); h.finishReceiver(app, 5, false);
        check(h.thread(app).registered == 5, "A disabled app setting suppressed retained receiver registrations");
    }

    private static ContentProviderHolder holder(Harness h, String suffix) {
        return holder(h, suffix, new Binder());
    }
    private static ContentProviderHolder holder(Harness h, String suffix, IBinder binder) {
        ProviderInfo info = h.pm.getProviderInfo(part("Provider" + suffix), 0, 0);
        ContentProviderHolder holder = new ContentProviderHolder(info);
        holder.provider = (IContentProvider)Proxy.newProxyInstance(PackageEnabledLifecycleFixture.class.getClassLoader(),
                new Class<?>[]{IContentProvider.class}, (proxy, method, args) -> {
                    if (method.getName().equals("asBinder")) return binder;
                    throw new AssertionError("Unexpected provider application invocation " + method);
                });
        return holder;
    }
    private static ContentProviderHolder acquire(Harness h, TestProcess client,
            String suffix, boolean stable) throws Exception {
        ContentProviderHolder[] result = new ContentProviderHolder[1];
        as(client, () -> result[0] = h.am.getContentProvider(client.thread, CLIENT, OWNER + ".provider" + suffix, 0, stable));
        return result[0];
    }

    /** Explicit fixture transport: no ordinary provider's liveness is substituted. */
    private static final class ProviderBinder extends Binder {
        final Harness h;
        boolean alive = true;
        final java.util.concurrent.atomic.AtomicInteger pings = new java.util.concurrent.atomic.AtomicInteger();
        Operation onPing;
        ProviderBinder(Harness h) { this.h = h; }
        @Override public boolean pingBinder() {
            h.unlocked(); pings.incrementAndGet();
            if (onPing != null) try { onPing.run(); }
            catch (Exception failure) { throw new AssertionError(failure); }
            return alive;
        }
    }
    private static void reportDeath(Harness h, TestProcess client, IBinder connection) throws Exception {
        as(client, () -> h.amApi.unstableProviderDied(connection));
    }
    private static void expectFailure(Class<? extends Throwable> expected, Operation operation,
            String reason) throws Exception {
        try { operation.run(); }
        catch (Throwable failure) {
            check(expected.isInstance(failure), reason + ": " + failure); return;
        }
        throw new AssertionError(reason + ": no exception");
    }

    private static void providerDeaths() throws Exception {
        Harness h = new Harness(); var owner = h.process(OWNER, true);
        var unstable = h.process(CLIENT, true); var stable = h.process(CLIENT, true);
        ProviderBinder transport = new ProviderBinder(h); transport.alive = false;
        ContentProviderHolder a = holder(h, "A", transport), b = holder(h, "B");
        as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(a, b)));
        ContentProviderHolder first = acquire(h, unstable, "A", false);
        ContentProviderHolder second = acquire(h, unstable, "B", false);
        ContentProviderHolder fixed = acquire(h, stable, "A", true);
        reportDeath(h, unstable, first.connection);
        check(!h.processes.contains(owner.record) && (Boolean)get(owner.record, "dead"),
                "Reported dead provider remained the current process");
        check(!owner.exiting(), "Unexpected provider death became an intentional force-stop");
        check(((Map<?, ?>)get(h.providers, "mPublished")).isEmpty(),
                "Dead provider process retained published authorities");
        check(h.thread(unstable).providerDeaths.size() == 2
                && h.thread(unstable).providerDeaths.containsAll(List.of(transport, b.provider.asBinder()))
                && h.thread(unstable).exits == 0, "Unstable clients did not receive provider invalidation");
        check(h.thread(stable).exits == 1 && stable.exiting(),
                "Stable dependent survived its provider's death");
        check(!h.am.refContentProvider(first.connection, 0, 0)
                && !h.am.refContentProvider(second.connection, 0, 0)
                && !h.am.refContentProvider(fixed.connection, 0, 0), "Dead connections survived cleanup");
        reportDeath(h, unstable, first.connection);
        invoke(h.atm, "processDied", new Class<?>[]{type("ActivityTaskManagerService$ProcessRecord")}, owner.record);
        check(h.thread(unstable).providerDeaths.size() == 2 && h.thread(stable).exits == 1
                && transport.pings.get() == 1, "Duplicate provider/process death notified a client twice");
        var replacement = h.process(OWNER, true); ContentProviderHolder fresh = holder(h, "A");
        as(replacement, () -> h.am.publishContentProviders(replacement.thread, List.of(fresh)));
        ContentProviderHolder recovered = acquire(h, unstable, "A", false);
        check(recovered != null && recovered.provider.asBinder() == fresh.provider.asBinder()
                && recovered.connection != first.connection, "Next acquisition reused the dead provider");
        reportDeath(h, unstable, first.connection);
        check(h.processes.contains(replacement.record) && h.am.refContentProvider(recovered.connection, 0, 0),
                "An old connection retired a replacement provider");

        Harness live = new Harness(); var liveOwner = live.process(OWNER, true);
        var client = live.process(CLIENT, true); var other = live.process(CLIENT, true);
        ProviderBinder alive = new ProviderBinder(live); ContentProviderHolder liveProvider = holder(live, "A", alive);
        as(liveOwner, () -> live.am.publishContentProviders(liveOwner.thread, List.of(liveProvider)));
        ContentProviderHolder liveRef = acquire(live, client, "A", false);
        reportDeath(live, client, liveRef.connection);
        check(alive.pings.get() == 1 && live.processes.contains(liveOwner.record)
                && live.am.refContentProvider(liveRef.connection, 0, 0), "A client's false report killed a live provider");
        expectFailure(NullPointerException.class, () -> reportDeath(live, client, null), "Null connection was accepted");
        expectFailure(IllegalArgumentException.class, () -> reportDeath(live, client, new Binder()),
                "An arbitrary Binder was accepted as a provider connection");
        expectFailure(SecurityException.class, () -> reportDeath(live, other, liveRef.connection),
                "Another process could report this client's provider");
        long identity = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long)(client.uid + 1) << 32) | client.pid);
            expectFailure(SecurityException.class, () -> live.amApi.unstableProviderDied(liveRef.connection),
                    "A mismatched Binder UID inherited a provider connection");
        } finally { Binder.restoreCallingIdentity(identity); }
        check(alive.pings.get() == 1, "Invalid ownership probed the provider");

        // A valid record with the same PID/UID must not confer the retired record's capability.
        int savedPid = nextPid; nextPid = client.pid;
        TestProcess newer;
        try { newer = live.process(CLIENT, true); }
        finally { nextPid = savedPid; }
        expectFailure(SecurityException.class, () -> reportDeath(live, newer, liveRef.connection),
                "PID reuse inherited a previous client's connection");
        check(alive.pings.get() == 1 && live.processes.contains(liveOwner.record), "PID reuse altered provider liveness");
        providerDeathRaces();
        concurrentProviderDeaths();

        Harness physical = new Harness(); var dead = physical.process(OWNER, true);
        var observer = physical.process(CLIENT, true); var dependent = physical.process(CLIENT, true);
        ContentProviderHolder provider = holder(physical, "A");
        as(dead, () -> physical.am.publishContentProviders(dead.thread, List.of(provider)));
        acquire(physical, observer, "A", false); acquire(physical, dependent, "A", true);
        invoke(physical.atm, "processDied", new Class<?>[]{type("ActivityTaskManagerService$ProcessRecord")}, dead.record);
        check(physical.thread(observer).providerDeaths.equals(List.of(provider.provider.asBinder()))
                && physical.thread(dependent).exits == 1,
                "Physical process death bypassed stable/unstable provider cleanup");
    }

    private static void providerDeathRaces() throws Exception {
        for (String race : List.of("publication", "owner-pid", "client-pid", "owner-code", "client-code", "release", "reentry")) {
            Harness h = new Harness(); var owner = h.process(OWNER, true); var client = h.process(CLIENT, true);
            ProviderBinder transport = new ProviderBinder(h); transport.alive = false;
            ContentProviderHolder old = holder(h, "A", transport), replacement = holder(h, "A");
            as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(old)));
            ContentProviderHolder held = acquire(h, client, "A", false);
            AtomicReference<TestProcess> newProcess = new AtomicReference<>();
            transport.onPing = () -> {
                if (race.equals("publication")) {
                    as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(replacement)));
                } else if (race.equals("owner-pid") || race.equals("client-pid")) {
                    int savedPid = nextPid; nextPid = race.equals("owner-pid") ? owner.pid : client.pid;
                    try { newProcess.set(h.process(race.equals("owner-pid") ? OWNER : CLIENT, true)); }
                    finally { nextPid = savedPid; }
                } else if (race.equals("owner-code") || race.equals("client-code")) {
                    Map<String, ApkPackage> packages = new java.util.LinkedHashMap<>((Map<String, ApkPackage>)get(h.pm, "mPackages"));
                    String replaced = race.equals("owner-code") ? OWNER : CLIENT;
                    packages.put(replaced, code(replaced)); set(h.pm, "mPackages", packages);
                } else if (race.equals("release")) {
                    as(client, () -> h.am.removeContentProvider(held.connection, false));
                } else {
                    // Reentrant physical death while the ping is in flight performs cleanup once.
                    invoke(h.atm, "processDied", new Class<?>[]{type("ActivityTaskManagerService$ProcessRecord")}, owner.record);
                }
            };
            reportDeath(h, client, held.connection);
            check(transport.pings.get() == 1, "Provider was probed repeatedly for race=" + race);
            if (race.equals("reentry")) {
                check(!h.processes.contains(owner.record) && h.thread(client).providerDeaths.equals(List.of(transport)),
                        "Reentrant death lost or duplicated notification");
            } else {
                check(h.processes.contains(owner.record) && !(Boolean)get(owner.record, "dead")
                        && h.thread(client).providerDeaths.isEmpty(), "Stale death retired a changed lifetime: " + race);
                if (newProcess.get() != null) check(h.processes.contains(newProcess.get().record)
                        && !(Boolean)get(newProcess.get().record, "dead"), "Death retired a replacement PID: " + race);
            }
        }
    }

    private static void concurrentProviderDeaths() throws Exception {
        Harness h = new Harness(); var owner = h.process(OWNER, true);
        var first = h.process(CLIENT, true); var second = h.process(CLIENT, true);
        ProviderBinder transport = new ProviderBinder(h); transport.alive = false;
        ContentProviderHolder published = holder(h, "A", transport);
        as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(published)));
        ContentProviderHolder a = acquire(h, first, "A", false), b = acquire(h, second, "A", false);
        CountDownLatch entered = new CountDownLatch(2), release = new CountDownLatch(1);
        transport.onPing = () -> { entered.countDown(); check(release.await(3, TimeUnit.SECONDS), "Concurrent ping not released"); };
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread one = new Thread(() -> {
            try { reportDeath(h, first, a.connection); }
            catch (Throwable error) { failure.compareAndSet(null, error); }
        }, "provider-death-one");
        Thread two = new Thread(() -> {
            try { reportDeath(h, second, b.connection); }
            catch (Throwable error) { failure.compareAndSet(null, error); }
        }, "provider-death-two");
        one.setDaemon(true); two.setDaemon(true); one.start(); two.start();
        try { check(entered.await(3, TimeUnit.SECONDS), "Concurrent pings held a lifecycle monitor"); }
        finally { release.countDown(); }
        one.join(3000); two.join(3000);
        check(!one.isAlive() && !two.isAlive() && failure.get() == null,
                "Concurrent reports did not terminate cleanly: " + failure.get());
        check(transport.pings.get() == 2 && !h.processes.contains(owner.record)
                && h.thread(first).providerDeaths.equals(List.of(transport))
                && h.thread(second).providerDeaths.equals(List.of(transport)),
                "Concurrent reports duplicated provider retirement or client callbacks");
    }
    private static void providers() throws Exception {
        Harness h = new Harness(); var owner = h.process(OWNER, true); h.start("ServiceB");
        var unstable = h.process(CLIENT, true); var stable = h.process(CLIENT, true);
        ContentProviderHolder a = holder(h, "A"), b = holder(h, "B");
        ContentProviderHolder wrongAuthority = holder(h, "A"); wrongAuthority.info.authority += ";unrelated.authority";
        try {
            as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(wrongAuthority)));
            throw new AssertionError("Admitted provider replaced its canonical authority");
        } catch (SecurityException expected) { check(true, "Authority validation rejected publication"); }
        ContentProviderHolder wrongUid = holder(h, "A");
        wrongUid.info.applicationInfo = new android.content.pm.ApplicationInfo(wrongUid.info.applicationInfo);
        wrongUid.info.applicationInfo.uid++;
        try {
            as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(wrongUid)));
            throw new AssertionError("Admitted provider forged its package UID");
        } catch (SecurityException expected) { check(true, "UID validation rejected publication"); }
        ContentProviderHolder unknown = holder(h, "A"); unknown.info.name += "Missing";
        as(owner, () -> h.am.publishContentProviders(owner.thread, java.util.Arrays.asList(null, unknown)));
        as(unstable, () -> h.am.publishContentProviders(unstable.thread, List.of(a)));
        check(((Map<?, ?>)get(h.providers, "mPublished")).isEmpty(),
                "Unrequested, incomplete or wrong-process provider publication changed authority ownership");
        as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(a, b)));
        ContentProviderHolder lost = acquire(h, unstable, "A", false), peer = acquire(h, unstable, "B", true);
        check(lost != null && peer != null && acquire(h, stable, "A", true) != null, "Provider references were not acquired");
        h.component("ProviderA", 2, KEEP);
        check(acquire(h, unstable, "A", false) == null && !h.am.refContentProvider(lost.connection, 0, 0),
                "Disabled provider remained acquireable before cleanup event");
        h.drain();
        check(!((Map<?, ?>)get(h.providers, "mPublished")).containsKey(OWNER + ".providerA")
                && ((Map<?, ?>)get(h.providers, "mPublished")).containsKey(OWNER + ".providerB"),
                "Provider cleanup removed a peer authority or retained the disabled one");
        check(h.thread(unstable).providerDeaths.equals(List.of(a.provider.asBinder())) && h.thread(unstable).exits == 0,
                "Unstable provider client did not receive scoped invalidation");
        check(h.thread(stable).exits == 1 && stable.exiting(), "Stable provider dependency was left alive");
        check(!h.am.refContentProvider(lost.connection, 0, 0) && h.am.refContentProvider(peer.connection, 0, 0),
                "Disabled provider reference survived or peer reference was retired");
        check(h.thread(owner).exits == 0 && !owner.exiting(), "Provider cleanup violated owner DONT_KILL_APP");
        as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(a)));
        check(!((Map<?, ?>)get(h.providers, "mPublished")).containsKey(OWNER + ".providerA"),
                "Late provider publication resurrected a disabled authority");

        h.component("ProviderA", 1, KEEP); h.drain();
        ContentProviderHolder fresh = holder(h, "A");
        h.thread(owner).onInstall = () -> as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(fresh)));
        ContentProviderHolder reacquired = acquire(h, unstable, "A", false);
        check(reacquired != null && reacquired.provider.asBinder() == fresh.provider.asBinder()
                && h.thread(owner).installs.equals(List.of(OWNER + ".ProviderA")),
                "Re-enabled provider was not installed in its retained live process");
        check(h.am.refContentProvider(peer.connection, 0, 0), "Provider re-install invalidated an enabled peer's reference");
        providerInstallRace();

        Harness waiting = new Harness(); var waitingClient = waiting.process(CLIENT, true);
        AtomicReference<Object> result = new AtomicReference<>();
        Thread request = new Thread(() -> {
            try { result.set(acquire(waiting, waitingClient, "A", false)); }
            catch (Throwable failure) { result.set(failure); }
        }, "enabled-provider-waiter");
        request.setDaemon(true); request.start();
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
        while (System.nanoTime() < deadline) {
            synchronized (waiting.atm) { if (!waiting.pending.isEmpty()) break; }
            Thread.sleep(2);
        }
        synchronized (waiting.atm) { check(!waiting.pending.isEmpty(), "Provider wait did not reserve a process"); }
        waiting.component("ProviderA", 2, KEEP); waiting.drain(); request.join(3000);
        check(!request.isAlive() && result.get() == null && waiting.pending.isEmpty(),
                "Disabled provider left its acquisition waiter or process admission pending: " + result.get());
    }

    private static void providerInstallRace() throws Exception {
        Harness h = new Harness(); var owner = h.process(OWNER, true); var client = h.process(CLIENT, true);
        h.component("ProviderA", 2, KEEP); h.drain(); h.component("ProviderA", 1, KEEP); h.drain();
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        ContentProviderHolder stale = holder(h, "A");
        h.thread(owner).onInstall = () -> {
            entered.countDown(); check(release.await(3, TimeUnit.SECONDS), "Provider install callback was not released");
            as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(stale)));
        };
        AtomicReference<Object> result = new AtomicReference<>();
        Thread request = new Thread(() -> {
            try { result.set(acquire(h, client, "A", false)); }
            catch (Throwable failure) { result.set(failure); }
        }, "enabled-provider-install");
        request.setDaemon(true); request.start();
        check(entered.await(3, TimeUnit.SECONDS), "Retained process never received scheduleInstallProvider");
        h.component("ProviderA", 2, KEEP); h.drain();
        h.component("ProviderA", 1, KEEP); h.drain(); release.countDown(); request.join(3000);
        check(!request.isAlive() && result.get() == null,
                "Re-enable revived a provider request canceled by actual cleanup: " + result.get());
        check(!((Map<?, ?>)get(h.providers, "mPublished")).containsKey(OWNER + ".providerA"),
                "Late publication recreated a removed pending provider after re-enable");
        ContentProviderHolder fresh = holder(h, "A");
        h.thread(owner).onInstall = () -> as(owner, () -> h.am.publishContentProviders(owner.thread, List.of(fresh)));
        check(acquire(h, client, "A", false).provider.asBinder() == fresh.provider.asBinder(),
                "A canceled installation blocked the next explicit provider acquisition");
        check(((java.util.Set<?>)get(h.providers, "mRequests")).isEmpty(), "Provider waiters leaked after completion");
    }

    @SuppressWarnings("unchecked")
    private static void activities() throws Exception {
        for (boolean eventFirst : List.of(false, true)) {
            Harness h = new Harness(); var app = h.process(OWNER, false); h.start("ServiceB");
            Object result = h.activity("ActivityB", 1, null, null, -1, "STOPPED");
            Object queued = h.activity("ActivityA", 2, app, result, 19, "INITIALIZING");
            h.component("ActivityA", 2, KEEP); if (eventFirst) h.drain(); h.ready(app); h.drain();
            check(get(queued, "state").toString().equals("DESTROYED") && (Boolean)get(queued, "finishing"),
                    "Disabled queued activity survived finishAttachApplication");
            List<android.app.ResultInfo> results = (List<android.app.ResultInfo>)get(result, "pendingResults");
            check(results.size() == 1 && results.get(0).mResultCode == Activity.RESULT_CANCELED,
                    "Disabled activity did not complete its pending result exactly once");
            check(!(Boolean)get(result, "finishing") && h.thread(app).exits == 0,
                    "Activity cleanup finished a peer or killed the process");
        }
        Harness h = new Harness(); var app = h.process(OWNER, true);
        Object activity = h.activity("ActivityA", 1, app, null, -1, "STOPPED");
        h.component("ActivityA", 2, KEEP); h.drain();
        check(h.thread(app).destroys.equals(List.of(get(activity, "token"))),
                "Disabled attached activity did not get lifecycle destruction");
        invoke(h.atm, "activityDestroyed", new Class<?>[]{type("ActivityTaskManagerService$ActivityRecord")}, activity);
        check(h.thread(app).exits == 0 && !app.exiting(), "Last activity cleanup violated DONT_KILL_APP");
    }

    /** Recording native boundary used only by the host runner; never controls an OS PID. */
    public static long childStartTime(int pid) { return pid == fakeChild ? 7001 : 0; }
    public static boolean killChild(int pid, long time) {
        check(pid == fakeChild && time == expectedKillTime, "Process retirement lost its retained generation");
        if (time != 7001) return false; // A reused PID belongs to a different native generation.
        childKilled = true; return true;
    }
    public static int exitCode(int pid) { return pid == fakeChild && !childKilled ? 259 : -1; }

    /** Only the host's recording native boundary may exercise synthetic PID identities. */
    @SuppressWarnings("unchecked")
    private static void terminationGeneration() throws Exception {
        try {
            for (long generation : new long[]{7001, 6001, 0}) {
                expectedKillTime = generation;
                Harness pending = new Harness(); fakeChild = nextPid++; childKilled = false;
                Object start = construct(type("ActivityTaskManagerService$PendingProcess"),
                        new Class<?>[]{String.class, String.class, String.class}, OWNER, OWNER, "fixture");
                set(start, "pid", fakeChild); set(start, "nativeStartTime", generation);
                ((Map<Long, Object>)get(pending.atm, "mStarting")).put(1L, start);
                invoke(pending.atm, "forceStopPackage", new Class<?>[]{String.class}, OWNER);
                check((Boolean)get(start, "cancelled") && childKilled == (generation == 7001),
                        "Force-stop killed a replacement PID or missed its admitted child");
                Harness running = new Harness(); TestProcess app = running.process(OWNER, true);
                fakeChild = app.pid; childKilled = false;
                set(app.record, "spawned", true); set(app.record, "nativeStartTime", generation);
                invoke(running.atm, "endProcesses", new Class<?>[]{List.class, long.class}, List.of(app.record), 0L);
                check(app.exiting() && childKilled == (generation == 7001),
                        "Process teardown killed a replacement PID or missed its admitted child");
                Harness shutdown = new Harness(); fakeChild = nextPid++; childKilled = false;
                Object stopping = construct(type("ActivityTaskManagerService$PendingProcess"),
                        new Class<?>[]{String.class, String.class, String.class}, OWNER, OWNER, "fixture");
                set(stopping, "pid", fakeChild); set(stopping, "nativeStartTime", generation);
                ((Map<Long, Object>)get(shutdown.atm, "mStarting")).put(1L, stopping);
                invoke(shutdown.atm, "endAllProcesses", new Class<?>[]{long.class}, 0L);
                check((Boolean)get(stopping, "cancelled") && childKilled == (generation == 7001),
                        "Shutdown killed a replacement PID or missed its admitted child");
                Harness timeout = new Harness(); TestProcess receiver = timeout.process(OWNER, true);
                fakeChild = receiver.pid; childKilled = false;
                set(receiver.record, "spawned", true); set(receiver.record, "nativeStartTime", generation);
                invoke(timeout.atm, "receiverTimedOut", new Class<?>[]{type("ActivityTaskManagerService$ProcessRecord")},
                        receiver.record);
                check(!timeout.processes.contains(receiver.record) && childKilled == (generation == 7001),
                        "Receiver timeout killed a replacement PID or missed its admitted child");
            }
        } finally { expectedKillTime = 7001; }
    }

    private static void startingProcess() throws Exception {
        Harness h = new Harness(); CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        fakeChild = nextPid++; childKilled = false;
        Class<?> starter = type("ActivityTaskManagerService$ProcessStarter");
        set(h.atm, "mStarter", Proxy.newProxyInstance(starter.getClassLoader(), new Class<?>[]{starter},
                (proxy, method, args) -> {
                    if (!method.getName().equals("start")) throw new AssertionError("Unexpected starter method " + method);
                    entered.countDown(); check(release.await(3, TimeUnit.SECONDS), "Blocked fake start was not released");
                    return fakeChild;
                }));
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread start = new Thread(() -> { try { h.start("ServiceA"); } catch (Throwable error) { failure.set(error); } });
        start.setDaemon(true); start.start(); check(entered.await(3, TimeUnit.SECONDS), "Fake native start was not entered");
        h.application(2, 0); h.drain();
        check(((Map<?, ?>)get(h.atm, "mStarting")).isEmpty() && h.pending.isEmpty(),
                "Disabled application retained an unauthenticated process reservation");
        release.countDown(); start.join(3000);
        check(!start.isAlive() && failure.get() == null && childKilled, "Late starter result was not retired using its retained identity: " + failure.get());
        h.application(1, KEEP); h.drain();
        set(h.atm, "mStarter", null); h.start("ServiceB");
        Thread.sleep(30); // Let the canceled generation's exit observer run.
        check(h.pending.size() == 1, "Old canceled process watcher cleared a newly enabled generation's work");

        Harness kept = new Harness(); CountDownLatch keepEntered = new CountDownLatch(1), keepRelease = new CountDownLatch(1);
        fakeChild = nextPid++; childKilled = false;
        set(kept.atm, "mStarter", Proxy.newProxyInstance(starter.getClassLoader(), new Class<?>[]{starter},
                (proxy, method, args) -> {
                    keepEntered.countDown(); check(keepRelease.await(3, TimeUnit.SECONDS), "KEEP child was not released");
                    return fakeChild;
                }));
        AtomicReference<Throwable> keepFailure = new AtomicReference<>();
        Thread keepStart = new Thread(() -> {
            try { kept.start("ServiceA"); } catch (Throwable error) { keepFailure.set(error); }
        }, "enabled-keep-start");
        keepStart.setDaemon(true); keepStart.start(); check(keepEntered.await(3, TimeUnit.SECONDS), "KEEP child never started");
        kept.application(2, KEEP); kept.drain();
        check(((Map<?, ?>)get(kept.atm, "mStarting")).size() == 1 && kept.pending.isEmpty(),
                "KEEP cleanup removed the admitted child instead of its disabled component work");
        keepRelease.countDown(); keepStart.join(3000);
        check(!keepStart.isAlive() && keepFailure.get() == null && !childKilled,
                "KEEP killed an admitted process when its delayed native start completed: " + keepFailure.get());
        kept.application(1, KEEP); kept.drain(); kept.start("ServiceB");
        check(kept.pending.size() == 1 && ((Map<?, ?>)get(kept.atm, "mStarting")).size() == 1,
                "Re-enable lost the retained child or resurrected canceled ServiceA work");
        childKilled = true; // Complete the recording-only child so its daemon observer can retire.
    }

    private static void activityGenerationRace() throws Exception {
        Harness h = new Harness(); CountDownLatch oldEntered = new CountDownLatch(1), oldRelease = new CountDownLatch(1);
        java.util.concurrent.atomic.AtomicInteger calls = new java.util.concurrent.atomic.AtomicInteger();
        fakeChild = nextPid++; childKilled = false;
        Class<?> starter = type("ActivityTaskManagerService$ProcessStarter");
        set(h.atm, "mStarter", Proxy.newProxyInstance(starter.getClassLoader(), new Class<?>[]{starter},
                (proxy, method, args) -> {
                    if (calls.getAndIncrement() == 0) {
                        oldEntered.countDown(); check(oldRelease.await(3, TimeUnit.SECONDS), "Old process start was not released");
                        return -1;
                    }
                    return fakeChild;
                }));
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread oldStart = new Thread(() -> {
            try { h.start("ServiceA"); } catch (Throwable error) { failure.set(error); }
        }, "enabled-old-process-start");
        oldStart.setDaemon(true); oldStart.start(); check(oldEntered.await(3, TimeUnit.SECONDS), "Old start never entered");
        h.component("ServiceA", 2, 0); // Commit restart; deliberately delay its event cleanup.
        check(h.atm.startActivity(null, "android", null, intent("ActivityB").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                null, null, null, -1, 0, null, null) == android.app.ActivityManager.START_SUCCESS,
                "Fresh enabled activity was not accepted during process restart");
        List<?> activities = (List<?>)get(h.atm, "mActivities");
        check(activities.size() == 1 && calls.get() == 2, "Fresh activity did not obtain a replacement process start");
        Object activity = activities.get(0);
        oldRelease.countDown(); oldStart.join(3000);
        check(!oldStart.isAlive() && failure.get() == null && activities.contains(activity)
                && !(Boolean)get(activity, "launchFailed") && get(activity, "state").toString().equals("INITIALIZING"),
                "Old failed process start removed the newly accepted activity: " + failure.get());
        h.drain();
        check(activities.contains(activity) && !childKilled,
                "Delayed old restart cleanup retired the fresh activity's process");
        childKilled = true;
    }

    public static int run(boolean useBinder, String group) throws Exception {
        binder = useBinder; checks = 0;
        if (group.equals("all") || group.equals("services")) services();
        if (group.equals("all") || group.equals("pending")) pending();
        if (group.equals("all") || group.equals("events")) events();
        if (group.equals("all") || group.equals("broadcasts")) broadcasts();
        if (group.equals("all") || group.equals("providers")) providers();
        if (group.equals("all") || group.equals("provider-death")) providerDeaths();
        if (group.equals("provider-install")) providerInstallRace();
        if (group.equals("all") || group.equals("activities")) activities();
        if (!useBinder && (group.equals("all") || group.equals("starting"))) startingProcess();
        if (!useBinder && (group.equals("all") || group.equals("activity-start"))) activityGenerationRace();
        if (!useBinder && (group.equals("all") || group.equals("termination"))) terminationGeneration();
        return checks;
    }
}
