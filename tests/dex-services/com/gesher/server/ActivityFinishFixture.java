/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Activity;
import android.app.ActivityClient;
import android.app.IActivityClientController;
import android.app.IApplicationThread;
import android.app.ResultInfo;
import android.app.servertransaction.ActivityResultItem;
import android.app.servertransaction.ActivityTransactionItem;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.ClientTransactionItem;
import android.app.servertransaction.DestroyActivityItem;
import android.app.servertransaction.PauseActivityItem;
import android.app.servertransaction.ResumeActivityItem;
import android.app.servertransaction.StopActivityItem;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import sun.misc.Unsafe;

/** Exercises production finish selection and lifecycle using real framework transaction objects. */
public final class ActivityFinishFixture {
    private static final String OWNER = "fixture.finish.owner";
    private static final int PID = 884231, UID = 12621;
    private static final Unsafe UNSAFE;
    private static boolean binder;
    private static int checks;
    static {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true);
            UNSAFE = (Unsafe) field.get(null);
        } catch (ReflectiveOperationException failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Operation { void run() throws Exception; }
    private static void check(boolean okay, String reason) {
        if (!okay) throw new AssertionError(reason);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
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
        Constructor<?> constructor = type.getDeclaredConstructor(parameters); constructor.setAccessible(true);
        try { return constructor.newInstance(arguments); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
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
    private static IBinder token(Object record) throws Exception { return (IBinder) get(record, "token"); }
    private static Activity activity(Object record) throws Exception {
        Activity activity = (Activity) UNSAFE.allocateInstance(Activity.class);
        field(Activity.class, "mToken").set(activity, token(record));
        return activity;
    }
    private static ApkPackage code() throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(OWNER, "/fixture/finish/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        // These records represent admitted activities; their synthetic package must
        // declare them when asynchronous admission rechecks installed enabled state.
        for (String name : List.of("Above", "Bottom", "Caller", "Child", "Last", "Match1", "Match2",
                "Middle", "NotChild", "Other", "OtherCode", "OtherParent", "OtherTask", "OtherWho",
                "Parent", "Pending", "Receiver", "Resuming", "Root", "RootWithResult", "Sibling", "Top")) {
            ParsedActivityImpl activity = new ParsedActivityImpl();
            activity.setPackageName(OWNER); activity.setName(OWNER + "." + name); activity.setEnabled(true);
            parsed.addActivity(activity);
        }
        parsed.hideAsFinal();
        ApkPackage code = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        set(code, "packageName", OWNER); set(code, "parsed", parsed); set(code, "mUid", UID);
        set(code, "apkPath", "/fixture/finish/base.apk"); set(code, "mDataDir", "/fixture/finish/data");
        set(code, "sharedLibraries", field(SharedLibraryGraph.class, "EMPTY").get(null));
        return code;
    }
    private static final class Event {
        final IBinder token;
        final ClientTransactionItem item;
        Event(IBinder token, ClientTransactionItem item) { this.token = token; this.item = item; }
    }
    private static final class AppThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        final List<Event> events = new ArrayList<>();
        final ArrayDeque<Event> pending = new ArrayDeque<>();
        IActivityClientController controller;
        boolean autoAck = true;
        Operation beforeTransaction;
        int exits;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleExit() { exits++; }
        @Override public void scheduleTransaction(ClientTransaction transaction) {
            try {
                if (beforeTransaction != null) beforeTransaction.run();
                for (ClientTransactionItem item : transaction.getTransactionItems()) {
                    Event event = new Event(item instanceof ActivityTransactionItem activity
                            ? activity.getActivityToken() : null, item);
                    events.add(event);
                    if (autoAck) acknowledge(event); else pending.addLast(event);
                }
            } catch (Exception failure) { throw new AssertionError("Lifecycle callback failed", failure); }
        }
        void acknowledge(Event event) throws Exception {
            if (event.item instanceof PauseActivityItem) controller.activityPaused(event.token);
            else if (event.item instanceof StopActivityItem)
                controller.activityStopped(event.token, null, null, null, null);
            else if (event.item instanceof DestroyActivityItem) controller.activityDestroyed(event.token);
            else if (event.item instanceof ResumeActivityItem) controller.activityResumed(event.token, false);
        }
        void drain() throws Exception {
            int remaining = 100;
            while (!pending.isEmpty()) {
                check(--remaining > 0, "Lifecycle did not settle"); acknowledge(pending.removeFirst());
            }
        }
        int count(Object record, Class<?> itemType) throws Exception {
            int count = 0;
            for (Event event : events) if (event.token == token(record) && itemType.isInstance(event.item)) count++;
            return count;
        }
        @SuppressWarnings("unchecked")
        List<ResultInfo> results(Object record) throws Exception {
            List<ResultInfo> results = new ArrayList<>();
            for (Event event : events) if (event.token == token(record) && event.item instanceof ActivityResultItem)
                results.addAll((List<ResultInfo>) field(ActivityResultItem.class, "mResultInfoList").get(event.item));
            return results;
        }
    }
    private static final class Harness {
        final Class<?> atmType = type("ActivityTaskManagerService");
        final Class<?> recordType = type("ActivityTaskManagerService$ActivityRecord");
        final Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
        final PackageManagerService pm;
        final Object atm, process;
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
            long original = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) UID << 32) | PID);
                process = construct(processType, new Class<?>[]{atmType, int.class, IApplicationThread.class}, atm, PID, thread);
            } finally { Binder.restoreCallingIdentity(original); }
            set(process, "packageName", OWNER); set(process, "processName", OWNER); set(process, "ready", true);
            ((List<Object>) get(atm, "mProcesses")).add(process);
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
            } else controller = implementation;
            thread.controller = controller;
            ActivityClient.setActivityClientController(controller);
        }
        Object record(String name, int task, String affinity, String state) throws Exception {
            return record(name, task, affinity, state, null, null, -1);
        }
        @SuppressWarnings("unchecked")
        Object record(String name, int task, String affinity, String state,
                Object parent, String resultWho, int request) throws Exception {
            ActivityInfo info = new ActivityInfo(); info.packageName = OWNER; info.name = OWNER + "." + name;
            info.applicationInfo = apk.applicationInfo(0); info.taskAffinity = affinity;
            info.screenOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
            Object record = construct(recordType, new Class<?>[]{atmType, ActivityInfo.class, Intent.class,
                    int.class, int.class, String.class, recordType, String.class, int.class},
                    atm, info, new Intent(), task, PID, OWNER, parent, resultWho, request);
            set(record, "app", process); set(record, "state", state(state));
            records.add(record); ((List<Object>) get(process, "activities")).add(record);
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
            } finally { Binder.restoreCallingIdentity(original); }
        }
        void gone(Object... selected) throws Exception {
            for (Object record : selected) {
                check(!records.contains(record) && get(record, "state") == state("DESTROYED"),
                        "Selected activity survived: " + ((ActivityInfo) get(record, "info")).name);
                check(thread.count(record, ResumeActivityItem.class) == 0,
                        "Activity selected for finish was resumed");
            }
        }
        void alive(Object... selected) throws Exception {
            for (Object record : selected) check(records.contains(record) && !(boolean) get(record, "finishing"),
                    "Unselected activity was finished");
        }
    }

    private static void removeTask() throws Exception {
        Harness h = new Harness();
        Object root = h.record("Root", 1, "a", "STOPPED");
        Object unrelated = h.record("OtherTask", 2, "a", "RESUMED");
        Object top = h.record("Top", 1, "a", "RESUMED");
        Object pending = h.record("Pending", 1, "a", "INITIALIZING");
        set(pending, "waitingFor", top);
        h.thread.beforeTransaction = () -> {
            for (Object record : List.of(root, top, pending))
                check((boolean) get(record, "finishing"), "Group was not marked before callback reentry");
        };
        Activity activity = activity(root);
        h.asApp(activity::finishAndRemoveTask);
        check(activity.isFinishing(), "Public root finishAndRemoveTask did not finish its Activity");
        h.gone(root, top, pending); h.alive(unrelated);
        check(h.thread.count(top, PauseActivityItem.class) == 1
                        && h.thread.count(top, StopActivityItem.class) == 1
                        && h.thread.count(top, DestroyActivityItem.class) == 1,
                "Resumed task member did not traverse pause, stop, destroy exactly once");
        check(h.thread.count(pending, DestroyActivityItem.class) == 0,
                "An unlaunched activity received a destroy transaction");

        Harness reentry = new Harness();
        Object resuming = reentry.record("Resuming", 4, "a", "RESUMING");
        Object last = reentry.record("Last", 4, "a", "STOPPED");
        set(last, "accountCompletion", (Runnable) () -> {
            try {
                check((boolean) get(resuming, "finishing"), "Completion ran before the group was marked");
                reentry.controller.activityResumed(token(resuming), false);
            } catch (Exception failure) { throw new AssertionError(failure); }
        });
        reentry.asApp(() -> activity(resuming).finishAndRemoveTask());
        reentry.gone(resuming, last);
        check(reentry.thread.count(resuming, PauseActivityItem.class) == 1,
                "A reentrant resume acknowledgement stranded a finishing activity");

        for (int flag : new int[]{Activity.DONT_FINISH_TASK_WITH_ACTIVITY,
                Activity.FINISH_TASK_WITH_ROOT_ACTIVITY, Activity.FINISH_TASK_WITH_ACTIVITY}) {
            Harness f = new Harness();
            Object bottom = f.record("Bottom", 11, "a", "RESUMED");
            Object caller = f.record("Caller", 11, "a", "STOPPED");
            Object other = f.record("Other", 12, "a", "RESUMED");
            f.asApp(() -> {
                if (flag == Activity.FINISH_TASK_WITH_ROOT_ACTIVITY) activity(caller).finishAndRemoveTask();
                else check(f.controller.finishActivity(token(caller), Activity.RESULT_CANCELED, null, flag),
                        "Finish rejected a live activity");
            });
            f.gone(caller); f.alive(other);
            if (flag == Activity.FINISH_TASK_WITH_ACTIVITY) f.gone(bottom); else f.alive(bottom);
        }
        // Task removal cancels any outstanding result, even if the finishing
        // activity supplied RESULT_OK/data. An ordinary finish retains them.
        for (int flag : new int[]{Activity.DONT_FINISH_TASK_WITH_ACTIVITY,
                Activity.FINISH_TASK_WITH_ROOT_ACTIVITY, Activity.FINISH_TASK_WITH_ACTIVITY}) {
            Harness f = new Harness();
            Object receiver = f.record("Receiver", 15, "b", "STOPPED");
            Object source = f.record("RootWithResult", 16, "a", "STOPPED", receiver, "result", 15);
            Object sibling = f.record("Sibling", 16, "a", "STOPPED");
            Intent data = new Intent("fixture.finish.result");
            f.asApp(() -> check(f.controller.finishActivity(token(source), Activity.RESULT_OK, data, flag),
                    "Result-bearing finish was rejected"));
            List<?> pendingResults = (List<?>) get(receiver, "pendingResults");
            check(pendingResults.size() == 1, "Finish did not complete the outstanding result");
            ResultInfo result = (ResultInfo) pendingResults.get(0);
            if (flag == Activity.DONT_FINISH_TASK_WITH_ACTIVITY) {
                check(result.mResultCode == Activity.RESULT_OK && result.mData != null
                                && data.getAction().equals(result.mData.getAction()),
                        "Ordinary activity finish lost its supplied result");
                f.alive(sibling);
            } else {
                check(result.mResultCode == Activity.RESULT_CANCELED && result.mData == null,
                        "Task removal forwarded a supplied result instead of canceling it");
                f.gone(sibling);
            }
            f.gone(source); f.alive(receiver);
        }
        System.out.println("Activity finish: root-only flag 1, unconditional flag 2, ordinary flag 0, interleaved tasks and unlaunched member passed");
    }

    private static void affinity() throws Exception {
        for (String[] affinities : new String[][]{{"a", "a", "a"}, {"a", "b", "b"},
                {"a", "b", "a"}, {null, null, null}}) {
            Harness h = new Harness();
            Object bottom = h.record("Bottom", 21, affinities[0], "STOPPED");
            Object other = h.record("Other", 22, affinities[2], "RESUMED");
            Object middle = h.record("Middle", 21, affinities[1], "STOPPED");
            Object caller = h.record("Caller", 21, affinities[2], "STOPPED");
            Object above = h.record("Above", 21, affinities[2], "RESUMED");
            Activity activity = activity(caller);
            h.asApp(activity::finishAffinity);
            check(activity.isFinishing(), "Public finishAffinity was not acknowledged");
            h.gone(caller); h.alive(other, above);
            boolean sameMiddle = java.util.Objects.equals(affinities[1], affinities[2]);
            if (sameMiddle) h.gone(middle); else h.alive(middle);
            if (sameMiddle && java.util.Objects.equals(affinities[0], affinities[2])) h.gone(bottom);
            else h.alive(bottom);
            int events = h.thread.events.size();
            h.asApp(() -> check(!h.controller.finishActivityAffinity(token(caller)),
                    "Destroyed affinity token was acknowledged as live"));
            check(events == h.thread.events.size(), "Repeated affinity finish emitted lifecycle work");
        }
        Harness h = new Harness();
        Object root = h.record("Root", 23, "a", "STOPPED");
        Object top = h.record("Top", 23, "a", "RESUMED");
        h.thread.autoAck = false;
        h.asApp(() -> activity(top).finishAffinity());
        check((boolean) get(root, "finishing") && (boolean) get(top, "finishing"),
                "Async affinity finish did not mark all members");
        int before = h.thread.events.size();
        h.asApp(() -> check(h.controller.finishActivityAffinity(token(top)), "Pending finish was not idempotent"));
        check(before == h.thread.events.size(), "Repeated pending affinity finish duplicated a transaction");
        h.asApp(h.thread::drain); h.gone(root, top);
        check(h.thread.exits == 1, "Last finished task did not release the idle app process once");
        System.out.println("Activity finish: contiguous/null affinity, above-caller preservation, async acknowledgements and repeat calls passed");
    }

    private static void subActivities() throws Exception {
        Harness h = new Harness();
        Object parent = h.record("Parent", 31, "a", "RESUMED");
        Object match1 = h.record("Match1", 32, "b", "STOPPED", parent, null, 7);
        Object otherCode = h.record("OtherCode", 32, "b", "STOPPED", parent, null, 8);
        Object otherWho = h.record("OtherWho", 32, "b", "STOPPED", parent, "fragment", 7);
        Object otherParent = h.record("OtherParent", 33, "c", "RESUMED");
        Object notChild = h.record("NotChild", 32, "b", "STOPPED", otherParent, null, 7);
        Object match2 = h.record("Match2", 32, "b", "STOPPED", parent, null, 7);
        h.asApp(() -> activity(parent).finishActivity(7));
        h.gone(match1, match2); h.alive(parent, otherCode, otherWho, otherParent, notChild);
        List<ResultInfo> results = h.thread.results(parent);
        check(results.size() == 2, "Already-resumed parent did not receive each canceled child result");
        for (ResultInfo result : results) check(result.mRequestCode == 7 && result.mResultWho == null
                        && result.mResultCode == Activity.RESULT_CANCELED && result.mData == null,
                "Canceled child result lost its routing fields");
        h.asApp(() -> activity(parent).finishActivity(7));
        check(h.thread.results(parent).size() == 2, "Repeated child cancellation delivered another result");
        h.asApp(() -> h.controller.finishSubActivity(token(parent), "fragment", 7));
        h.gone(otherWho); h.alive(parent, otherCode, otherParent, notChild);
        results = h.thread.results(parent);
        check(results.size() == 3 && "fragment".equals(results.get(2).mResultWho),
                "Non-null resultWho did not match exactly");

        Harness paused = new Harness();
        Object receiver = paused.record("Receiver", 41, "a", "STOPPED");
        Object child = paused.record("Child", 41, "a", "RESUMED", receiver, null, 9);
        paused.asApp(() -> activity(receiver).finishActivity(9));
        paused.gone(child); paused.alive(receiver);
        check(paused.thread.results(receiver).size() == 1
                        && paused.thread.count(receiver, ResumeActivityItem.class) == 1,
                "Stopped result receiver was not resumed with its canceled result");
        int resultIndex = -1, resumeIndex = -1;
        for (int i = 0; i < paused.thread.events.size(); i++) {
            Event event = paused.thread.events.get(i);
            if (event.token != token(receiver)) continue;
            if (event.item instanceof ActivityResultItem) resultIndex = i;
            if (event.item instanceof ResumeActivityItem) resumeIndex = i;
        }
        check(resultIndex >= 0 && resultIndex < resumeIndex, "Result arrived after the parent's resume");
        System.out.println("Activity finish: child parent/resultWho/request routing, canceled results and result-before-resume passed");
    }

    public static int run(boolean realBinder, String group) throws Exception {
        binder = realBinder; checks = 0;
        if (group.equals("all") || group.equals("task")) removeTask();
        if (group.equals("all") || group.equals("affinity")) affinity();
        if (group.equals("all") || group.equals("sub")) subActivities();
        return checks;
    }
}
