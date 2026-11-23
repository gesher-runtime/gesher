/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.app.IApplicationThread;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.ResumeActivityItem;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PermissionInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;

/** Real caller validation and controller hooks with the established host JNI boundary.
 * APK/process construction reuses the existing AppOps fixture. Binder Parcel dispatch
 * is deliberately not simulated: outgoing callbacks are delayed and inspected as policy state. */
public final class SensorAccessLifecycleHost {
    private static final int UID = 12772, PID = 887720;
    private static int checks;
    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name); field.setAccessible(true); return field;
    }
    private static Object get(Object object, String name) throws Exception { return field(object.getClass(), name).get(object); }
    private static void set(Object object, String name, Object value) throws Exception { field(object.getClass(), name).set(object, value); }
    private static Object call(Object target, String name, Class<?>[] signature, Object... args) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name, signature); method.setAccessible(true);
        try { return method.invoke(target, args); }
        catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception exception) throw exception;
            throw failure;
        }
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    private static SensorAccessPolicy.State state(Object session) throws Exception {
        return ((SensorAccessPolicy.Subscription)get(session, "subscription")).snapshot();
    }
    private static final class Token extends Binder {
        final List<IBinder.DeathRecipient> deaths = new ArrayList<>();
        @Override public void linkToDeath(IBinder.DeathRecipient recipient, int flags) { deaths.add(recipient); }
        @Override public boolean unlinkToDeath(IBinder.DeathRecipient recipient, int flags) { return deaths.remove(recipient); }
        void die() { for (var recipient : new ArrayList<>(deaths)) recipient.binderDied(); }
    }

    /** Real device-mode launch and policy, with only the client transaction sink isolated.
     * The sink never acknowledges resume: an app may register/poll inside onResume. */
    private static final class LaunchFixture implements AutoCloseable {
        final PackageManagerService pm;
        final ActivityTaskManagerService atm;
        final Object process, record, session;
        final ApkPackage owner;
        final Class<?> recordType = Class.forName("com.gesher.server.ActivityTaskManagerService$ActivityRecord");
        final Class<?> stateType = Class.forName("com.gesher.server.ActivityTaskManagerService$State");
        final Token token = new Token();
        final ExecutorService notifications;
        int transactions;
        boolean rejectDispatch, resumeSent;
        Runnable duringDispatch;

        @SuppressWarnings("unchecked") LaunchFixture() throws Exception {
            Class<?> fixture = Class.forName("gesher.services.AppOpsContractTest$Harness");
            Method packageFactory = fixture.getEnclosingClass().getDeclaredMethod("pkg",
                    String.class, int.class, byte.class, int.class);
            packageFactory.setAccessible(true);
            owner = (ApkPackage)packageFactory.invoke(null, "fixture.sensor.launch", UID, (byte)7, 36);
            // Complete declared manifest input before real PM admission.
            PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(owner.packageName, owner.apkPath);
            parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
            ParsedActivityImpl component = new ParsedActivityImpl();
            component.setPackageName(owner.packageName); component.setName(owner.packageName + ".Sensor");
            component.setEnabled(true); parsed.addActivity(component); parsed.hideAsFinal();
            set(owner, "parsed", parsed);
            pm = new PackageManagerService(List.of(owner), false);
            set(pm, "mPackageEventSender", (java.util.function.Consumer<Intent>)event -> { });
            Object catalog = get(pm.permissions(), "mCatalog");
            set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            atm = new ActivityTaskManagerService(pm);
            ActivityManagerService am = new ActivityManagerService(atm, pm, null);
            IApplicationThread receiver = new IApplicationThread.Default() {
                private final Binder binder = new Binder();
                @Override public IBinder asBinder() { return binder; }
                @Override public void scheduleTransaction(ClientTransaction transaction) throws RemoteException {
                    if (rejectDispatch) throw new RemoteException("Explicit failed lifecycle enqueue");
                    transactions++;
                    for (var item : transaction.getTransactionItems())
                        if (item instanceof ResumeActivityItem) resumeSent = true;
                    Runnable action = duringDispatch;
                    duringDispatch = null;
                    if (action != null) action.run();
                    // No ActivityClient.activityResumed: that is sent after onResume returns.
                }
            };
            Class<?> processType = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
            Constructor<?> processConstructor = processType.getDeclaredConstructor(
                    ActivityTaskManagerService.class, int.class, IApplicationThread.class);
            processConstructor.setAccessible(true);
            long identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)UID << 32) | PID);
                process = processConstructor.newInstance(atm, PID, receiver);
            } finally { Binder.restoreCallingIdentity(identity); }
            set(process, "packageName", owner.packageName); set(process, "processName", owner.packageName);
            set(process, "packageCode", owner); set(process, "ready", true);
            ((List<Object>)get(atm, "mProcesses")).add(process);
            ActivityInfo info = new ActivityInfo();
            info.packageName = owner.packageName; info.name = component.getName();
            info.applicationInfo = owner.applicationInfo(0);
            info.screenOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
            Constructor<?> activity = recordType.getDeclaredConstructor(ActivityTaskManagerService.class,
                    ActivityInfo.class, Intent.class, int.class, int.class, String.class,
                    recordType, String.class, int.class);
            activity.setAccessible(true);
            record = activity.newInstance(atm, info, new Intent(), 1, PID, owner.packageName, null, null, -1);
            set(record, "app", process);
            ((List<Object>)get(atm, "mActivities")).add(record);
            ((List<Object>)get(process, "activities")).add(record);
            ((List<Integer>)get(atm, "mTaskOrder")).add(1);
            SensorAccessService access = am.sensorAccess();
            notifications = (ExecutorService)get(access, "notifications");
            CountDownLatch blocked = new CountDownLatch(1);
            notifications.execute(() -> { blocked.countDown(); try { new CountDownLatch(1).await(); }
                catch (InterruptedException done) { Thread.currentThread().interrupt(); } });
            blocked.await();
            identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)UID << 32) | PID);
                session = call(access, "openSubscription", new Class<?>[]{IBinder.class}, token);
            } finally { Binder.restoreCallingIdentity(identity); }
        }
        void start() throws Exception { call(atm, "realStartActivity", new Class<?>[]{recordType}, record); }
        boolean active() throws Exception { return (state(session).flags() & SensorAccessPolicy.ACTIVE) != 0; }
        void refresh() throws Exception { call(atm, "updateIdle", new Class<?>[0]); }
        Object otherRecord() throws Exception {
            Constructor<?> constructor = recordType.getDeclaredConstructor(ActivityTaskManagerService.class,
                    ActivityInfo.class, Intent.class, int.class, int.class, String.class,
                    recordType, String.class, int.class);
            constructor.setAccessible(true);
            return constructor.newInstance(atm, get(record, "info"), new Intent(), 1,
                    PID, owner.packageName, null, null, -1);
        }
        @SuppressWarnings({"rawtypes", "unchecked"}) void setActivityState(String value) throws Exception {
            set(record, "state", Enum.valueOf((Class)stateType, value));
        }
        void excluded(Object target, String field, Object denied, Object restored, String label) throws Exception {
            set(target, field, denied); refresh(); check(!active(), label);
            set(target, field, restored); refresh(); check(active(), "Current visible launch recovers after " + label);
        }
        @Override public void close() {
            token.die();
            notifications.shutdownNow();
        }
    }

    @SuppressWarnings("unchecked") private static void launchBeforeAcknowledgement() throws Exception {
        try (LaunchFixture h = new LaunchFixture()) {
            check(!h.active(), "Pre-launch subscription is inactive");
            long before = state(h.session).generation();
            h.start();
            check(h.transactions == 1 && h.resumeSent, "Real launch enqueues the resume transaction");
            check(h.active(), "Successful visible launch publishes active policy without a resume acknowledgement");
            check(get(h.record, "state").toString().equals("LAUNCHING")
                    && state(h.session).generation() > before,
                    "Existing inactive subscriber observes activation while client onResume is outstanding");
            h.excluded(h.record, "launchingPaused", true, false, "Paused launch is not resume authority");
            h.excluded(h.record, "nativeUiCount", 1, 0, "Native-covered launch is inactive");
            h.excluded(h.record, "chooserCoverCount", 1, 0, "Chooser-covered launch is inactive");
            h.excluded(h.record, "finishing", true, false, "Finishing launch is inactive");
            h.excluded(h.record, "waitingFor", h.record, null, "Launch waiting for another activity is inactive");
            h.excluded(h.process, "ready", false, true, "Unready process is inactive");
            h.excluded(h.process, "dead", true, false, "Dead process is inactive");
            h.excluded(h.process, "exiting", true, false, "Exiting process is inactive");
            h.excluded(h.process, "pidSuperseded", true, false, "Retired PID is inactive");
            h.excluded(h.process, "packageCode", null, h.owner, "Unadmitted process is inactive");
            h.excluded(h.record, "targetCode", null, h.owner, "Retired target code is inactive");
            ((PackageImpl)h.owner.parsed).setEnabled(false); h.refresh();
            check(!h.active(), "Disabled package does not gain early launch authority");
            ((PackageImpl)h.owner.parsed).setEnabled(true); h.refresh();
            check(h.active(), "Enabled current package restores eligible launch");
            ParsedActivityImpl component = (ParsedActivityImpl)h.owner.parsed.getActivities().get(0);
            component.setEnabled(false); h.refresh();
            check(!h.active(), "Disabled activity does not gain early launch authority");
            component.setEnabled(true); h.refresh();
            check(h.active(), "Enabled current activity restores eligible launch");
            Set<Integer> hidden = (Set<Integer>)get(h.atm, "mHiddenTasks");
            hidden.add(1); h.refresh(); check(!h.active(), "Hidden task launch is inactive");
            hidden.remove(1); h.refresh(); check(h.active(), "Visible launch returns after task reveal");
            List<Object> records = (List<Object>)get(h.atm, "mActivities");
            Object above = h.otherRecord();
            records.add(above); h.refresh(); check(!h.active(), "Covered non-top launch is inactive");
            records.remove(above); h.refresh(); check(h.active(), "Top launch returns after cover removal");
            // A removed record cannot retain authority through its process.activities entry.
            records.remove(h.record); h.refresh(); check(!h.active(), "Detached launch is inactive");
            records.add(h.record); h.refresh(); check(h.active(), "Current launch record restored");
            call(h.atm, "finish", new Class<?>[]{h.recordType, String.class, int.class, Intent.class},
                    h.record, "sensor-contract", 0, null);
            check(!h.active(), "Real cancellation revokes an outstanding launch without client acknowledgement");
        }
        try (LaunchFixture h = new LaunchFixture()) {
            h.rejectDispatch = true;
            h.start();
            check(h.transactions == 0 && !h.active(), "Failed enqueue does not activate a launch");
            h.refresh(); check(!h.active(), "A later state refresh cannot admit the failed enqueue");
        }
        try (LaunchFixture h = new LaunchFixture()) {
            h.start(); check(h.active(), "Initial attempt activates before failed re-enqueue control");
            // The retained-record retry boundary resets these existing lifecycle
            // fields. The production attempt must discard its own previous success.
            h.setActivityState("INITIALIZING"); set(h.record, "launchScheduled", false);
            h.rejectDispatch = true;
            h.start();
            check(!h.active(), "Failed re-enqueue immediately clears the prior successful launch policy");
            h.refresh(); check(!h.active(), "Old successful dispatch cannot survive a failed new attempt");
        }
        try (LaunchFixture h = new LaunchFixture()) {
            set(h.record, "chooserCoverCount", 1);
            h.start();
            check(h.transactions == 1 && !h.resumeSent && !h.active(),
                    "Actual launch into paused state does not gain early resume authority");
        }
        for (String change : List.of("cancel", "hide", "retire")) {
            try (LaunchFixture h = new LaunchFixture()) {
                h.duringDispatch = () -> {
                    try {
                        if (change.equals("cancel"))
                            call(h.atm, "finish", new Class<?>[]{h.recordType, String.class, int.class, Intent.class},
                                    h.record, "canceled-during-dispatch", 0, null);
                        else if (change.equals("hide")) ((Set<Integer>)get(h.atm, "mHiddenTasks")).add(1);
                        else set(h.process, "ready", false);
                    } catch (Exception failure) { throw new AssertionError(failure); }
                };
                h.start();
                check(!h.active(), "Successful enqueue cannot revive target changed during dispatch: " + change);
            }
        }
        try (LaunchFixture h = new LaunchFixture()) {
            h.start(); check(h.active(), "Stop fixture reaches active launch");
            h.setActivityState("STOPPING");
            call(h.atm, "activityStopped", new Class<?>[]{h.recordType, android.os.Bundle.class,
                    android.os.PersistableBundle.class}, h.record, null, null);
            check(!h.active(), "Real stopped acknowledgement revokes the final visible activity");
            call(h.atm, "resume", new Class<?>[]{h.recordType}, h.record);
            check(h.active() && get(h.record, "state").toString().equals("RESUMING"),
                    "Successful re-resume publishes active state without waiting for client acknowledgement");
            h.setActivityState("STOPPING");
            call(h.atm, "activityStopped", new Class<?>[]{h.recordType, android.os.Bundle.class,
                    android.os.PersistableBundle.class}, h.record, null, null);
            h.rejectDispatch = true;
            call(h.atm, "resume", new Class<?>[]{h.recordType}, h.record);
            check(!h.active() && get(h.record, "state").toString().equals("STOPPED"),
                    "Failed re-resume preserves the stopped state and does not grant delivery");
            h.refresh(); check(!h.active(), "Failed resume stays denied on later refresh");
        }
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    private static void lifecycle(int targetSdk, boolean permission, boolean debuggable) throws Exception {
        Class<?> fixture = Class.forName("gesher.services.AppOpsContractTest$Harness");
        Constructor<?> constructor = fixture.getDeclaredConstructor(Integer.class, String[].class);
        constructor.setAccessible(true);
        Object harness = constructor.newInstance(targetSdk, permission
                ? new String[]{Manifest.permission.HIGH_SAMPLING_RATE_SENSORS} : new String[0]);
        var pm = (PackageManagerService)get(harness, "pm");
        var atm = (ActivityTaskManagerService)get(harness, "atm");
        var am = (ActivityManagerService)get(harness, "am");
        Object process = get(harness, "process");
        ApkPackage owner = (ApkPackage)get(harness, "owner");
        ((PackageImpl)owner.parsed).setDebuggable(debuggable);
        if (permission) {
            Object catalog = get(pm.permissions(), "mCatalog");
            PermissionInfo info = new PermissionInfo();
            info.name = Manifest.permission.HIGH_SAMPLING_RATE_SENSORS;
            info.packageName = "android"; info.protectionLevel = PermissionInfo.PROTECTION_NORMAL;
            ((Map<String, PermissionInfo>)get(catalog, "mFramework")).put(info.name, info);
            call(pm.permissions(), "initializePackages", new Class<?>[0]);
        }
        SensorAccessService access = am.sensorAccess();
        ExecutorService notifications = (ExecutorService)get(access, "notifications");
        CountDownLatch blocked = new CountDownLatch(1);
        notifications.execute(() -> { blocked.countDown(); try { new CountDownLatch(1).await(); }
            catch (InterruptedException done) { Thread.currentThread().interrupt(); } });
        blocked.await();
        long identity = Binder.clearCallingIdentity();
        Token token = new Token();
        try {
            Binder.restoreCallingIdentity(((long)UID << 32) | PID);
            Object session = call(access, "openSubscription", new Class<?>[]{IBinder.class}, token);
            check((state(session).flags() & 1) != 0 && (state(session).flags() & 2) == 0,
                    "Authenticated pre-resume process is available but delivery-inactive");
            check(((state(session).flags() & 4) != 0) == (targetSdk >= 31 && !permission),
                    "Rate policy derives from actual target SDK and installed manifest grant");
            check(((state(session).flags() & SensorAccessPolicy.DEBUGGABLE) != 0) == debuggable,
                    "Debug policy derives from authenticated package metadata");
            Token duplicate = new Token();
            try {
                call(access, "openSubscription", new Class<?>[]{IBinder.class}, duplicate);
                throw new AssertionError("One process acquired a duplicate policy subscription");
            } catch (IllegalStateException expected) {
                check(duplicate.deaths.isEmpty() && token.deaths.size() == 1,
                        "Duplicate subscription adds no callback and preserves the original");
            }
            Class stateType = Class.forName("com.gesher.server.ActivityTaskManagerService$State");
            Object stopped = Enum.valueOf(stateType, "STOPPED"), resumed = Enum.valueOf(stateType, "RESUMED");
            set(process, "inProcessActivityState", stopped);
            call(atm, "inProcessActivityState", new Class<?>[]{process.getClass(), stateType}, process, resumed);
            long first = state(session).generation();
            check((state(session).flags() & 2) != 0, "Real resumed-activity hook grants delivery");
            call(atm, "inProcessActivityState", new Class<?>[]{process.getClass(), stateType}, process, stopped);
            check((state(session).flags() & 2) == 0 && state(session).generation() == first + 1,
                    "Real stopped-activity hook records denial before deferred callback dispatch");
            call(atm, "inProcessActivityState", new Class<?>[]{process.getClass(), stateType}, process, resumed);
            check(state(session).generation() == first + 2, "Rapid foreground return retains denial epoch");
            if (targetSdk >= 31 && !permission) {
                call(atm, "inProcessActivityState", new Class<?>[]{process.getClass(), stateType}, process, stopped);
                Object services = get(am, "mServices");
                set(services, "mNotifications", new NotificationManagerService());
                android.content.pm.ServiceInfo info = new android.content.pm.ServiceInfo();
                info.packageName = owner.packageName; info.name = owner.packageName + ".SensorForeground";
                Class<?> record = Class.forName("com.gesher.server.ActiveServices$ServiceRecord");
                Constructor<?> serviceCtor = record.getDeclaredConstructor(android.content.pm.ServiceInfo.class, ApkPackage.class);
                serviceCtor.setAccessible(true);
                Object service = serviceCtor.newInstance(info, owner);
                var component = new android.content.ComponentName(info.packageName, info.name);
                ((Map<Object, Object>)get(services, "mServices")).put(component, service);
                set(service, "app", process); set(service, "foregroundId", 41);
                call(services, "notifySensorStateChanged", new Class<?>[0]);
                check((state(session).flags() & 2) != 0, "Actual foreground-service controller admits inactive activity UID");
                call(services, "setServiceForeground", new Class<?>[]{android.content.ComponentName.class,
                        IBinder.class, int.class, android.app.Notification.class, int.class, int.class},
                        component, service, 0, null, 0, 0);
                check((state(session).flags() & 2) == 0, "Production stopForeground hook revokes the final active source");
            }
            Binder.restoreCallingIdentity(((long)(UID + 1) << 32) | PID);
            try {
                call(access, "openSubscription", new Class<?>[]{IBinder.class}, new Token());
                throw new AssertionError("Mismatched UID accepted");
            } catch (SecurityException expected) { checks++; }
            Binder.restoreCallingIdentity(((long)UID << 32) | (PID + 1));
            try {
                call(access, "openSubscription", new Class<?>[]{IBinder.class}, new Token());
                throw new AssertionError("Unattached PID accepted");
            } catch (SecurityException expected) { checks++; }
            Binder.restoreCallingIdentity(((long)UID << 32) | PID);
            token.die();
            check(state(session).flags() == 0 && token.deaths.isEmpty(), "Callback death retires authority and its death registration");
            Token replacement = new Token();
            Object renewed = call(access, "openSubscription", new Class<?>[]{IBinder.class}, replacement);
            check((state(renewed).flags() & SensorAccessPolicy.AVAILABLE) != 0,
                    "Process can acquire a fresh subscription after its old callback dies");
            replacement.die();
        } finally {
            Binder.restoreCallingIdentity(identity);
            notifications.shutdownNow();
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        field(locales, "sIs24Hour").set(null, Boolean.TRUE);
        field(locales, "sLocales").set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        lifecycle(36, false, false);
        lifecycle(30, false, false);
        lifecycle(36, true, false);
        lifecycle(36, false, true);
        launchBeforeAcknowledgement();
        System.out.println("Sensor caller identity + real lifecycle hooks: " + checks + " checks passed");
    }
}
