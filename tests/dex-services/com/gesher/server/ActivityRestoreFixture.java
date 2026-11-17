/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Activity;
import android.app.ActivityClient;
import android.app.ActivityManager;
import android.app.IActivityClientController;
import android.app.IApplicationThread;
import android.app.ResultInfo;
import android.app.servertransaction.ActivityTransactionItem;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.ClientTransactionItem;
import android.app.servertransaction.DestroyActivityItem;
import android.app.servertransaction.LaunchActivityItem;
import android.app.servertransaction.PauseActivityItem;
import android.app.servertransaction.ResumeActivityItem;
import android.app.servertransaction.StopActivityItem;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.PersistableBundle;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import sun.misc.Unsafe;

/** Exercises Activity state restoration, task persistence, and process death lifecycle contracts. */
public final class ActivityRestoreFixture {
    private static final String OWNER = "fixture.restore.owner";
    private static final int PID = 771234, UID = 11987;
    private static final Unsafe UNSAFE;
    private static boolean binder;
    private static int checks;

    static {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe");
            field.setAccessible(true);
            UNSAFE = (Unsafe) field.get(null);
        } catch (ReflectiveOperationException failure) {
            throw new ExceptionInInitializerError(failure);
        }
    }

    private interface Operation {
        void run() throws Exception;
    }

    private static void check(boolean okay, String reason) {
        if (!okay) throw new AssertionError(reason);
        checks++;
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name);
        result.setAccessible(true);
        return result;
    }

    private static Object get(Object target, String name) throws Exception {
        return field(target.getClass(), name).get(target);
    }

    private static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }

    private static Class<?> type(String name) throws Exception {
        return Class.forName("com.gesher.server." + name);
    }

    private static Object construct(Class<?> type, Class<?>[] parameters, Object... arguments) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(parameters);
        constructor.setAccessible(true);
        try {
            return constructor.newInstance(arguments);
        } catch (InvocationTargetException failure) {
            return rethrow(failure.getCause());
        }
    }

    private static Object rethrow(Throwable failure) throws Exception {
        if (failure instanceof Error error) throw error;
        if (failure instanceof Exception exception) throw exception;
        throw new AssertionError(failure);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static Object state(String name) throws Exception {
        return Enum.valueOf((Class) type("ActivityTaskManagerService$State"), name);
    }

    private static IBinder token(Object record) throws Exception {
        return (IBinder) get(record, "token");
    }

    private static Activity activity(Object record) throws Exception {
        Activity activity = (Activity) UNSAFE.allocateInstance(Activity.class);
        field(Activity.class, "mToken").set(activity, token(record));
        return activity;
    }

    private static ApkPackage code() throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(OWNER, "/fixture/restore/base.apk");
        parsed.setEnabled(true);
        parsed.setTargetSdkVersion(36);
        for (String name : List.of("MainAct", "RootAct", "TopAct", "TransientAct", "ParentAct", "ChildAct", "TaskRoot", "BottomAct")) {
            ParsedActivityImpl activity = new ParsedActivityImpl();
            activity.setPackageName(OWNER); activity.setName(OWNER + "." + name);
            activity.setEnabled(true); activity.setExported(true); parsed.addActivity(activity);
        }
        parsed.hideAsFinal();
        ApkPackage code = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        set(code, "packageName", OWNER);
        set(code, "parsed", parsed);
        set(code, "mUid", UID);
        set(code, "apkPath", "/fixture/restore/base.apk");
        set(code, "mDataDir", "/fixture/restore/data");
        set(code, "sharedLibraries", field(SharedLibraryGraph.class, "EMPTY").get(null));
        return code;
    }

    private static final class Event {
        final IBinder token;
        final ClientTransactionItem item;
        Event(IBinder token, ClientTransactionItem item) {
            this.token = token;
            this.item = item;
        }
    }

    private static final class AppThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        final List<Event> events = new ArrayList<>();
        final ArrayDeque<Event> pending = new ArrayDeque<>();
        IActivityClientController controller;
        boolean autoAck = true;
        boolean failNextTransaction;
        Operation onExit;
        @Override public void scheduleExit() {
            try { if (onExit != null) onExit.run(); }
            catch (Exception failure) { throw new AssertionError(failure); }
        }

        @Override public IBinder asBinder() { return binder; }

        @Override public void scheduleTransaction(ClientTransaction transaction) throws RemoteException {
            if (failNextTransaction) { failNextTransaction = false; throw new RemoteException("isolated failed restore IPC"); }
            try {
                for (ClientTransactionItem item : transaction.getTransactionItems()) {
                    IBinder token = null;
                    if (item instanceof ActivityTransactionItem activity) {
                        token = activity.getActivityToken();
                    } else if (item instanceof LaunchActivityItem launch) {
                        token = launch.getActivityToken();
                    }
                    Event event = new Event(token, item);
                    events.add(event);
                    if (autoAck) acknowledge(event); else pending.addLast(event);
                }
            } catch (Exception failure) {
                throw new AssertionError("Lifecycle callback failed", failure);
            }
        }

        void acknowledge(Event event) throws Exception {
            if (event.item instanceof PauseActivityItem) {
                controller.activityPaused(event.token);
            } else if (event.item instanceof StopActivityItem) {
                controller.activityStopped(event.token, null, null, null, null);
            } else if (event.item instanceof DestroyActivityItem) {
                controller.activityDestroyed(event.token);
            } else if (event.item instanceof ResumeActivityItem) {
                controller.activityResumed(event.token, false);
            }
        }

        LaunchActivityItem findLaunchItem(Object record) throws Exception {
            IBinder target = token(record);
            for (Event event : events) {
                if (event.token == target && event.item instanceof LaunchActivityItem launch) {
                    return launch;
                }
            }
            return null;
        }
    }

    private static final class Harness {
        final Class<?> atmType = type("ActivityTaskManagerService");
        final Class<?> recordType = type("ActivityTaskManagerService$ActivityRecord");
        final Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
        final PackageManagerService pm;
        final Object atm;
        final Object process;
        final ApkPackage apk = code();
        final List<Object> records;
        final AppThread thread = new AppThread();
        final IActivityClientController controller;

        @SuppressWarnings("unchecked")
        Harness() throws Exception {
            pm = new PackageManagerService(List.of(apk));
            atm = construct(atmType, new Class<?>[]{PackageManagerService.class}, pm);
            construct(type("ActivityManagerService"), new Class<?>[]{atmType,
                    PackageManagerService.class, type("NotificationManagerService")}, atm, pm, null);
            process = newProcess(PID, thread);
            records = (List<Object>) get(atm, "mActivities");
            IActivityClientController implementation = (IActivityClientController) get(atm, "mClientImpl");
            if (binder) {
                StandIn.setStrict(true);
                IBinder target = StandIn.of(IActivityClientController.class, implementation).asBinder();
                controller = IActivityClientController.Stub.asInterface(new Binder() {
                    @Override public IInterface queryLocalInterface(String descriptor) { return null; }
                    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                            throws RemoteException { return target.transact(code, data, reply, flags); }
                });
            } else {
                controller = implementation;
            }
            thread.controller = controller;
            ActivityClient.setActivityClientController(controller);
        }

        Object newProcess(int pid, AppThread appThread) throws Exception {
            long original = Binder.clearCallingIdentity();
            Object proc;
            try {
                Binder.restoreCallingIdentity(((long) UID << 32) | pid);
                proc = construct(processType, new Class<?>[]{atmType, int.class, IApplicationThread.class},
                        atm, pid, appThread);
            } finally {
                Binder.restoreCallingIdentity(original);
            }
            set(proc, "packageName", OWNER);
            set(proc, "processName", OWNER);
            set(proc, "ready", true);
            ((List<Object>) get(atm, "mProcesses")).add(proc);
            appThread.controller = controller;
            return proc;
        }

        Object record(String name, int task, String affinity, String state) throws Exception {
            return record(name, task, affinity, state, process, null, null, -1);
        }

        Object record(String name, int task, String affinity, String state,
                Object parent, String resultWho, int request) throws Exception {
            return record(name, task, affinity, state, process, parent, resultWho, request);
        }

        @SuppressWarnings("unchecked")
        Object record(String name, int task, String affinity, String state, Object targetProc,
                Object parent, String resultWho, int request) throws Exception {
            ActivityInfo info = new ActivityInfo();
            info.packageName = OWNER;
            info.name = OWNER + "." + name;
            info.applicationInfo = apk.applicationInfo(0);
            info.taskAffinity = affinity;
            info.screenOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
            int pid = (int) get(targetProc, "pid");
            Object record = construct(recordType, new Class<?>[]{atmType, ActivityInfo.class, Intent.class,
                    int.class, int.class, String.class, recordType, String.class, int.class},
                    atm, info, new Intent(), task, pid, OWNER, parent, resultWho, request);
            set(record, "app", targetProc);
            set(record, "state", state(state));
            records.add(record);
            ((List<Object>) get(targetProc, "activities")).add(record);
            List<Integer> order = (List<Integer>) get(atm, "mTaskOrder");
            if (!order.contains(task)) order.add(task);
            return record;
        }

        void asApp(Operation action) throws Exception {
            long original = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) UID << 32) | PID);
                check(Binder.getCallingUid() == UID && Binder.getCallingPid() == PID, "Caller identity unavailable");
                action.run();
            } finally {
                Binder.restoreCallingIdentity(original);
            }
        }

        void processDied(Object proc) throws Exception {
            Method method = atmType.getDeclaredMethod("processDied", processType);
            method.setAccessible(true);
            method.invoke(atm, proc);
        }

        void activityStopped(Object record, Bundle state, PersistableBundle persistentState) throws Exception {
            Method method = atmType.getDeclaredMethod("activityStopped", recordType, Bundle.class, PersistableBundle.class);
            method.setAccessible(true);
            method.invoke(atm, record, state, persistentState);
        }

        void realStartActivity(Object record) throws Exception {
            Method method = atmType.getDeclaredMethod("realStartActivity", recordType);
            method.setAccessible(true);
            method.invoke(atm, record);
        }

        void attach(Object record, Object proc) throws Exception {
            Method method = atmType.getDeclaredMethod("attach", recordType, processType);
            method.setAccessible(true);
            method.invoke(atm, record, proc);
            set(record, "state", state("INITIALIZING"));
        }

        void moveTaskToFront(int taskId) throws Exception {
            Method method = atmType.getDeclaredMethod("moveTaskToFront", IApplicationThread.class,
                    String.class, int.class, int.class, Bundle.class);
            method.setAccessible(true);
            method.invoke(atm, null, OWNER, taskId, 0, null);
        }

        @SuppressWarnings("unchecked")
        List<ActivityManager.RunningTaskInfo> getTasks(int maxNum) throws Exception {
            Method method = atmType.getDeclaredMethod("getTasks", int.class, boolean.class, boolean.class, int.class);
            method.setAccessible(true);
            return (List<ActivityManager.RunningTaskInfo>) method.invoke(atm, maxNum, false, false, 0);
        }
    }

    private static void testSavedStatePreservedAcrossProcessDeath() throws Exception {
        Harness h = new Harness();
        Object r1 = h.record("MainAct", 1, "a", "RESUMED");

        Bundle saved = new Bundle();
        saved.putString("saved_text", "user_entered_form_data");
        PersistableBundle persist = new PersistableBundle();
        persist.putString("persist_doc_id", "doc_98765");

        h.activityStopped(r1, saved, persist);
        check(get(r1, "savedState") == saved, "activityStopped did not retain savedState on record");
        check(get(r1, "persistentState") == persist, "activityStopped did not retain persistentState on record");
        h.activityStopped(r1, null, null);
        check(get(r1, "savedState") == saved, "Stop timeout discarded saved state");
        check(get(r1, "persistentState") == persist, "Null stop discarded persistent state");


        // Simulate process death
        h.processDied(h.process);

        // Verification after process death:
        check(get(r1, "app") == null, "Record still held killed process reference");
        check(get(r1, "state") == state("STOPPED"), "Record state was not STOPPED after process death");
        check(h.records.contains(r1), "Non-finishing record was removed from mActivities");

        Bundle retainedState = (Bundle) get(r1, "savedState");
        check(retainedState != null, "savedState was lost on process death");
        check("user_entered_form_data".equals(retainedState.getString("saved_text")),
                "savedState contents were corrupted or cleared on process death");

        PersistableBundle retainedPersist = (PersistableBundle) get(r1, "persistentState");
        check(retainedPersist != null, "persistentState was lost on process death");
        check("doc_98765".equals(retainedPersist.getString("persist_doc_id")),
                "persistentState contents were corrupted on process death");

        List<ActivityManager.RunningTaskInfo> tasks = h.getTasks(10);
        check(tasks.size() == 1, "Expected 1 task in running tasks");
        check(tasks.get(0).taskId == 1, "Task ID mismatch");
        check(tasks.get(0).numActivities == 1, "Task numActivities mismatch");
        check(!tasks.get(0).isRunning, "Task should not be running when process has died");

        // Relaunch simulation with a new process
        AppThread newThread = new AppThread();
        newThread.autoAck = false; // Process can die again before acknowledging resume.
        Object newProcess = h.newProcess(PID + 1, newThread);
        h.attach(r1, newProcess);
        h.realStartActivity(r1);

        LaunchActivityItem item = newThread.findLaunchItem(r1);
        check(item != null, "LaunchActivityItem was not scheduled for relaunched record");

        Bundle deliveredState = (Bundle) field(LaunchActivityItem.class, "mState").get(item);
        check(deliveredState != null, "LaunchActivityItem delivered null mState");
        check("user_entered_form_data".equals(deliveredState.getString("saved_text")),
                "Delivered state bundle did not match original saved state");

        PersistableBundle deliveredPersist = (PersistableBundle) field(LaunchActivityItem.class, "mPersistentState").get(item);
        check(deliveredPersist != null, "LaunchActivityItem delivered null mPersistentState");
        check("doc_98765".equals(deliveredPersist.getString("persist_doc_id")),
                "Delivered persistable bundle did not match original persistent state");

        check(get(r1, "savedState") == saved, "Server snapshot must survive launch until a later stop replaces it");
        check(get(r1, "persistentState") == persist, "Persistent snapshot must survive launch");

        tasks = h.getTasks(10);
        check(tasks.get(0).isRunning, "Task should be running after relaunch");
        boolean staleRejected = false;
        try { h.asApp(() -> h.controller.activityStopped(token(r1), new Bundle(), null, null, null)); }
        catch (SecurityException expected) { staleRejected = true; }
        check(staleRejected, "Retired process could overwrite relaunched activity snapshot");
        // A second death before another onStop must still recover the last committed snapshot.
        h.processDied(newProcess);
        check(get(r1, "savedState") == saved, "Repeated death lost the last stopped snapshot");
        check(get(r1, "launchGeneration") == null, "Dead process generation leaked into restart");
        check(!(boolean)get(r1, "launchScheduled"), "Restart retained an already scheduled launch");
        AppThread thirdThread = new AppThread();
        h.newProcess(PID + 10, thirdThread);
        h.moveTaskToFront(1);
        LaunchActivityItem repeated = thirdThread.findLaunchItem(r1);
        check(repeated != null, "Repeated death failed to relaunch");
        check("user_entered_form_data".equals(((Bundle)field(LaunchActivityItem.class, "mState").get(repeated)).getString("saved_text")),
                "Repeated relaunch omitted the last snapshot");
        check(get(r1, "savedState") == null, "Acknowledged resume retained stale state");

        System.out.println("Activity state restore: savedState bundle and persistable retention across process death and delivery passed");
    }

    private static void testFinishingActivitiesCleanedUpWhileNonFinishingPreserved() throws Exception {
        Harness h = new Harness();
        // Task 2: Root (non-finishing) and Top (finishing)
        Object root = h.record("RootAct", 2, "a", "STOPPED");
        Object top = h.record("TopAct", 2, "a", "RESUMED");
        set(top, "finishing", true);

        // Task 3: Only finishing activity
        Object transientAct = h.record("TransientAct", 3, "a", "RESUMED");
        set(transientAct, "finishing", true);

        Bundle rootState = new Bundle();
        rootState.putString("root_data", "root_value");
        h.activityStopped(root, rootState, null);

        h.processDied(h.process);

        // Non-finishing root in Task 2 must be preserved
        check(h.records.contains(root), "Non-finishing root was removed on process death");
        check(get(root, "app") == null, "Root still has dead app reference");
        check(get(root, "state") == state("STOPPED"), "Root state was not STOPPED");
        check(get(root, "savedState") == rootState, "Root lost savedState");

        // Finishing activities must be destroyed and removed
        check(!h.records.contains(top), "Finishing top activity survived process death in mActivities");
        check(get(top, "state") == state("DESTROYED"), "Finishing top activity was not DESTROYED");

        check(!h.records.contains(transientAct), "Finishing transient activity survived in mActivities");
        check(get(transientAct, "state") == state("DESTROYED"), "Finishing transient activity was not DESTROYED");

        @SuppressWarnings("unchecked")
        List<Integer> taskOrder = (List<Integer>) get(h.atm, "mTaskOrder");
        check(taskOrder.contains(2), "Task 2 was prematurely removed from mTaskOrder");
        check(!taskOrder.contains(3), "Empty task 3 was not cleaned up from mTaskOrder");

        List<ActivityManager.RunningTaskInfo> tasks = h.getTasks(10);
        check(tasks.size() == 1, "Only task 2 should remain in task list, found " + tasks.size());
        check(tasks.get(0).taskId == 2, "Remaining task ID mismatch");
        check(tasks.get(0).numActivities == 1, "Task 2 activity count mismatch");
        System.out.println("Activity state restore: finishing activity cleanup vs non-finishing task preservation passed");
    }

    private static void testPendingResultsPreservedAcrossProcessDeath() throws Exception {
        Harness h = new Harness();
        Object parent = h.record("ParentAct", 4, "a", "STOPPED");

        Bundle parentState = new Bundle();
        parentState.putString("screen", "parent_step_2");
        h.activityStopped(parent, parentState, null);

        // Parent's process dies while stopped
        h.processDied(h.process);

        check(h.records.contains(parent), "Parent activity lost from mActivities");
        check(get(parent, "app") == null, "Parent app reference not cleared");

        // Child runs in a separate process and returns a result to the dead parent
        AppThread childThread = new AppThread();
        Object childProc = h.newProcess(PID + 2, childThread);
        Object child = h.record("ChildAct", 4, "a", "RESUMED", childProc, parent, "result_who", 88);

        Intent resultData = new Intent("fixture.restore.RESULT_ACTION").putExtra("code", 12345);
        long original = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) UID << 32) | (PID + 2));
            h.controller.finishActivity(token(child), Activity.RESULT_OK, resultData,
                    Activity.DONT_FINISH_TASK_WITH_ACTIVITY);
        } finally {
            Binder.restoreCallingIdentity(original);
        }

        // In activityPaused: child is finishing, parent was dead (app == null) in task 4.
        // The server attaches parent to childProc and immediately launches parent via realStartActivity
        // with both savedState and the pending result from child!
        LaunchActivityItem item = childThread.findLaunchItem(parent);
        check(item != null, "LaunchActivityItem was not scheduled for relaunched parent");

        Bundle deliveredState = (Bundle) field(LaunchActivityItem.class, "mState").get(item);
        check(deliveredState != null && "parent_step_2".equals(deliveredState.getString("screen")),
                "Saved state not delivered alongside results in LaunchActivityItem");

        @SuppressWarnings("unchecked")
        List<ResultInfo> deliveredResults = (List<ResultInfo>) field(LaunchActivityItem.class, "mPendingResults").get(item);
        check(deliveredResults != null && deliveredResults.size() == 1,
                "Delivered pending results count mismatch in LaunchActivityItem");
        ResultInfo delivered = deliveredResults.get(0);
        check(delivered.mRequestCode == 88, "Delivered requestCode mismatch");
        check(delivered.mResultCode == Activity.RESULT_OK, "Delivered resultCode mismatch");
        check("result_who".equals(delivered.mResultWho), "Delivered resultWho mismatch");
        check(delivered.mData != null && delivered.mData.getIntExtra("code", 0) == 12345,
                "Delivered result intent extra mismatch");

        @SuppressWarnings("unchecked")
        List<ResultInfo> parentResults = (List<ResultInfo>) get(parent, "pendingResults");
        check(parentResults.isEmpty(), "pendingResults on record was not cleared after delivery in LaunchActivityItem");
        System.out.println("Activity state restore: pendingResults retention and LaunchActivityItem delivery passed");
    }

    private static void testMoveTaskToFrontRelaunch() throws Exception {
        Harness h = new Harness();
        Object root = h.record("TaskRoot", 5, "a", "STOPPED");
        Bundle state = new Bundle();
        state.putString("session_token", "token_xyz");
        h.activityStopped(root, state, null);

        h.processDied(h.process);
        check(get(root, "app") == null, "App not cleared after process death");

        // Start a fresh ready process for OWNER
        AppThread newThread = new AppThread();
        Object newProcess = h.newProcess(PID + 3, newThread);

        // Move the dead task to front: should attach and realStartActivity
        h.moveTaskToFront(5);

        check(get(root, "app") == newProcess, "moveTaskToFront did not attach ready process to top activity");
        LaunchActivityItem item = newThread.findLaunchItem(root);
        check(item != null, "moveTaskToFront did not launch top activity in dead task");

        Bundle delivered = (Bundle) field(LaunchActivityItem.class, "mState").get(item);
        check(delivered != null && "token_xyz".equals(delivered.getString("session_token")),
                "moveTaskToFront LaunchActivityItem did not contain saved state");
        System.out.println("Activity state restore: moveTaskToFront relaunch with state passed");
    }

    private static void testActivityPausedFinishingRelaunchesUnderlying() throws Exception {
        Harness h = new Harness();
        Object bottom = h.record("BottomAct", 6, "a", "STOPPED");
        Bundle state = new Bundle();
        state.putString("bottom_key", "bottom_val");
        h.activityStopped(bottom, state, null);

        // Process dies while bottom is stopped
        h.processDied(h.process);

        // New process launches and runs a ChildAct on top of the same task 6
        AppThread thread2 = new AppThread();
        Object proc2 = h.newProcess(PID + 4, thread2);
        Object child = h.record("ChildAct", 6, "a", "RESUMED", proc2, null, null, -1);

        // Child finishes and pauses
        long original = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) UID << 32) | (PID + 4));
            h.controller.finishActivity(token(child), Activity.RESULT_CANCELED, null,
                    Activity.DONT_FINISH_TASK_WITH_ACTIVITY);
        } finally {
            Binder.restoreCallingIdentity(original);
        }

        // In activityPaused: child is finishing, bottom is topOf(6) with app == null.
        // Since proc2 is ready, bottom is attached to proc2 and launched with state!
        check(get(bottom, "app") == proc2, "Bottom activity was not attached to available ready process");
        LaunchActivityItem item = thread2.findLaunchItem(bottom);
        check(item != null, "Bottom activity was not relaunched when child finished");

        Bundle delivered = (Bundle) field(LaunchActivityItem.class, "mState").get(item);
        check(delivered != null && "bottom_val".equals(delivered.getString("bottom_key")),
                "Bottom activity LaunchActivityItem did not restore savedState");
        System.out.println("Activity state restore: finishing child relaunches underlying dead activity with state passed");
    }

    private static void testFailedRestoreDelivery() throws Exception {
        Harness h = new Harness();
        Object record = h.record("MainAct", 10, "a", "STOPPED");
        Bundle saved = new Bundle(); saved.putString("retry", "snapshot");
        h.activityStopped(record, saved, null);
        h.processDied(h.process);
        AppThread failing = new AppThread(); failing.failNextTransaction = true;
        Object failedProcess = h.newProcess(PID + 20, failing);
        h.attach(record, failedProcess); h.realStartActivity(record);
        check(failing.findLaunchItem(record) == null, "Failed IPC delivered a launch");
        check(get(record, "savedState") == saved, "Failed IPC consumed snapshot");
        h.processDied(failedProcess);
        check(!(boolean)get(record, "launchScheduled"), "Failed launch stranded scheduled flag");
        AppThread retry = new AppThread(); h.newProcess(PID + 21, retry);
        h.moveTaskToFront(10);
        LaunchActivityItem delivered = retry.findLaunchItem(record);
        check(delivered != null, "Failed launch could not be relaunched after process death");
        check("snapshot".equals(((Bundle)field(LaunchActivityItem.class, "mState").get(delivered)).getString("retry")),
                "Retry lost original snapshot");
        check(get(record, "savedState") == null, "Successful resumed acknowledgment did not release snapshot");
    }

    private static void testForceStopLiveSavedRecord() throws Exception {
        Harness h = new Harness();
        Object original = h.record("MainAct", 1, "a", "STOPPED");
        set(original, "savedState", new Bundle());
        Object[] replacement = new Object[1];
        h.thread.onExit = () -> {
            check((boolean)get(original, "finishing"), "Force stop did not retire live activity before exit callback");
            h.processDied(h.process);
            Object next = h.newProcess(PID + 1, new AppThread());
            replacement[0] = h.record("Replacement", 2, "b", "RESUMED", next, null, null, -1);
        };
        Method stop = h.atmType.getDeclaredMethod("forceStopPackage", String.class);
        stop.setAccessible(true); stop.invoke(h.atm, OWNER);
        check(!h.records.contains(original), "Force stop restored saved live activity");
        check(h.records.size() == 1 && h.records.contains(replacement[0]), "Force stop consumed concurrent replacement");
        check(!(boolean)get(get(replacement[0], "app"), "exiting"), "Force stop killed replacement generation");
    }

    public static int run(boolean realBinder, String group) throws Exception {
        binder = realBinder;
        checks = 0;
        if (group.equals("all") || group.equals("saved")) { testSavedStatePreservedAcrossProcessDeath(); testFailedRestoreDelivery(); }
        if (group.equals("all") || group.equals("cleanup")) {
            testFinishingActivitiesCleanedUpWhileNonFinishingPreserved();
            testForceStopLiveSavedRecord();
        }
        if (group.equals("all") || group.equals("results")) testPendingResultsPreservedAcrossProcessDeath();
        if (group.equals("all") || group.equals("movetask")) testMoveTaskToFrontRelaunch();
        if (group.equals("all") || group.equals("paused")) testActivityPausedFinishingRelaunchesUnderlying();
        return checks;
    }
}
