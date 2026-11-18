/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Activity;
import android.app.ActivityClient;
import android.app.ActivityManager;
import android.app.IActivityClientController;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.Instrumentation;
import android.app.PendingIntent;
import android.app.ResultInfo;
import android.app.servertransaction.ActivityResultItem;
import android.app.servertransaction.ActivityTransactionItem;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.ClientTransactionItem;
import android.app.servertransaction.DestroyActivityItem;
import android.app.servertransaction.PauseActivityItem;
import android.app.servertransaction.ResumeActivityItem;
import android.app.servertransaction.StopActivityItem;
import android.content.ActivityNotFoundException;
import android.content.ComponentName;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.Signature;
import android.net.Uri;
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
import java.util.function.Predicate;
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
        set(code, "mSigningIdentityChecked", true);
        set(code, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{1})});
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
        boolean rejectNextResult;
        Operation beforeTransaction;
        int exits;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleExit() { exits++; }
        @Override public void scheduleTransaction(ClientTransaction transaction) throws RemoteException {
            if (rejectNextResult && transaction.getTransactionItems().stream()
                    .anyMatch(ActivityResultItem.class::isInstance)) {
                rejectNextResult = false;
                throw new RemoteException("Fixture recipient disconnected before result delivery");
            }
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

    private static void externalUrlResults() throws Exception {
        Harness h = new Harness();
        Object source = h.record("Caller", 51, "a", "RESUMED");
        IBinder sourceToken = token(source);
        ActivityTaskManagerService atm = (ActivityTaskManagerService) h.atm;
        Field opener = field(h.atmType, "sOpenExternalUrl");
        Object previousOpener = opener.get(null);
        List<String> attempts = new ArrayList<>();
        try {
            opener.set(null, (Predicate<String>) url -> {
                attempts.add(url);
                return url.startsWith("https:");
            });
            Intent web = new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.com/page"));
            Intent mail = new Intent(Intent.ACTION_VIEW, Uri.parse("mailto:help@example.com"));
            h.asApp(() -> {
                int failedStart = atm.startActivity(h.thread, OWNER, null, mail, null, sourceToken,
                        "compose", 7, 0, null, null);
                check(failedStart == ActivityManager.START_INTENT_NOT_RESOLVED,
                        "A failed Windows launch reported success");
                try {
                    Instrumentation.checkStartActivityResult(failedStart, mail);
                    throw new AssertionError("A failed launch must surface as ActivityNotFoundException");
                } catch (ActivityNotFoundException expected) {
                    // The same exception a caller of Activity.startActivity receives.
                }
                check(atm.startActivity(h.thread, OWNER, null, web, null, sourceToken,
                                "browse", 8, 0, null, null) == ActivityManager.START_SUCCESS,
                        "An accepted Windows launch did not report success");
                check(h.thread.results(source).size() == 1,
                        "Shell acceptance fabricated an ordinary activity-result completion");
                check(atm.startActivity(h.thread, OWNER, null,
                                new Intent(web).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), null,
                                sourceToken, "browseNewTask", 9, 0, null, null) == ActivityManager.START_SUCCESS,
                        "An accepted NEW_TASK Windows launch did not report success");
                check(atm.startActivity(h.thread, OWNER, null, mail, null, sourceToken,
                                null, -1, 0, null, null) == ActivityManager.START_INTENT_NOT_RESOLVED,
                        "A failed Windows launch without a result request reported success");
            });
            check(attempts.equals(List.of(mail.getDataString(), web.getDataString(),
                            web.getDataString(), mail.getDataString())),
                    "Windows received incorrect URLs or more than one call per request: " + attempts);
            List<ResultInfo> results = h.thread.results(source);
            check(results.size() == 2, "Each failed or NEW_TASK result request must cancel once: " + results);
            for (int i = 0; i < results.size(); i++) {
                ResultInfo result = results.get(i);
                check(result.mResultCode == Activity.RESULT_CANCELED && result.mData == null
                                && result.mCallerToken == null
                                && result.mRequestCode == List.of(7, 9).get(i)
                                && List.of("compose", "browseNewTask").get(i).equals(result.mResultWho),
                        "External URL cancellation lost its routing fields: " + result);
            }
            check(h.records.equals(List.of(source)), "External URLs created an Android activity");

            opener.set(null, (Predicate<String>) url -> {
                try {
                    set(h.process, "exiting", true);
                } catch (Exception failure) {
                    throw new AssertionError(failure);
                }
                return true;
            });
            h.asApp(() -> check(atm.startActivity(h.thread, OWNER, null,
                            new Intent(web).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), null, sourceToken,
                            "afterExit", 10, 0, null, null) == ActivityManager.START_SUCCESS,
                    "The Windows launch was accepted before the caller exited"));
            check(h.thread.results(source).size() == 2, "Exit during the launch left a stale result");
        } finally {
            opener.set(null, previousOpener);
        }
        System.out.println("External URL results: Windows launch status and canceled result routing passed");
    }

    @SuppressWarnings("unchecked")
    private static void externalForwardedResults() throws Exception {
        for (int mode = 0; mode < 5; mode++) {
            Harness h = new Harness();
            Object parent = h.record("Parent", 61, "a", "RESUMED");
            Object child = h.record("Child", 61, "a", "RESUMED", parent, "original-route", 71);
            AppThread recipientThread = h.thread;
            if (mode == 4) {
                recipientThread = new AppThread();
                recipientThread.controller = h.controller;
                Object parentProcess = construct(h.processType,
                        new Class<?>[]{h.atmType, int.class, IApplicationThread.class},
                        h.atm, PID + 1, recipientThread);
                set(parentProcess, "packageName", OWNER); set(parentProcess, "processName", OWNER + ":parent");
                set(parentProcess, "uid", UID); set(parentProcess, "ready", true);
                ((List<Object>) get(h.atm, "mProcesses")).add(parentProcess);
                ((List<Object>) get(h.process, "activities")).remove(parent);
                ((List<Object>) get(parentProcess, "activities")).add(parent);
                set(parent, "app", parentProcess);
            }
            ActivityTaskManagerService atm = (ActivityTaskManagerService) h.atm;
            Field opener = field(h.atmType, "sOpenExternalUrl");
            Object previous = opener.get(null);
            List<String> attempts = new ArrayList<>();
            final int which = mode;
            int[] status = {Integer.MIN_VALUE};
            try {
                opener.set(null, (Predicate<String>) url -> {
                    attempts.add(url); return which != 0 && which != 4;
                });
                Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.com/forwarded"))
                        .addFlags(Intent.FLAG_ACTIVITY_FORWARD_RESULT);
                if (mode == 2) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                h.asApp(() -> status[0] = atm.startActivity(h.thread, OWNER, null, intent, null,
                        token(child), "new-route", which == 1 ? 72 : -1, 0, null, null));
                List<ResultInfo> results = recipientThread.results(parent);
                if (mode == 1) {
                    check(status[0] == ActivityManager.START_FORWARD_AND_REQUEST_CONFLICT,
                            "Forwarding plus a new request did not reject the conflict");
                    check(attempts.isEmpty() && results.isEmpty() && h.thread.results(child).isEmpty(),
                            "Conflicting request invoked the shell or sent a result");
                    check(get(child, "resultTo") == parent, "Rejected forwarding consumed the original route");
                } else {
                    boolean failed = mode == 0 || mode == 4;
                    boolean canceled = failed || mode == 2;
                    check(status[0] == (failed ? ActivityManager.START_INTENT_NOT_RESOLVED
                            : ActivityManager.START_SUCCESS), "Forwarded launch lost the actual shell status");
                    check(attempts.size() == 1 && results.size() == (canceled ? 1 : 0),
                            "Forwarded route lost or fabricated a result");
                    if (canceled) {
                        ResultInfo result = results.get(0);
                        check(result.mRequestCode == 71 && "original-route".equals(result.mResultWho)
                                        && result.mResultCode == Activity.RESULT_CANCELED
                                        && result.mCallerToken == null && result.mData == null,
                                "Forwarding replaced the original recipient fields");
                    }
                    check(get(child, "resultTo") == null, "Forwarding did not detach the intermediate route");
                    check(h.controller.getCallingActivity(token(child)) == null
                                    && h.controller.getCallingPackage(token(child)) == null,
                            "Calling activity queries retained a transferred result route");
                    h.asApp(() -> h.controller.finishActivity(token(child), Activity.RESULT_OK, null, 0));
                    check(recipientThread.results(parent).size() == (canceled ? 1 : 0),
                            "Finishing the intermediate activity sent a duplicate result");
                }
            } finally { opener.set(null, previous); }
        }

        Harness h = new Harness();
        Object parent = h.record("Parent", 62, "a", "STOPPED");
        Object child = h.record("Child", 62, "a", "RESUMED", parent, "same-route", 73);
        Object sibling = h.record("Sibling", 62, "a", "RESUMED", parent, "same-route", 73);
        h.thread.autoAck = false;
        h.asApp(() -> {
            h.controller.finishActivity(token(child), Activity.RESULT_OK, null, 0);
            h.controller.finishActivity(token(sibling), Activity.RESULT_OK, null, 0);
        });
        List<ResultInfo> pending = (List<ResultInfo>) get(parent, "pendingResults");
        check(pending.size() == 2, "Fixture did not queue both ordinary child results");
        ResultInfo childResult = pending.get(0);
        ResultInfo siblingResult = pending.get(1);
        Object callerState = get(parent, "callerState");
        var callerSnapshot = callerState.getClass().getDeclaredMethod("get", IBinder.class);
        callerSnapshot.setAccessible(true);
        check(callerSnapshot.invoke(callerState, childResult.mCallerToken) != null,
                "Fixture did not retain the queued caller snapshot");
        Field opener = field(h.atmType, "sOpenExternalUrl");
        Object previous = opener.get(null);
        try {
            opener.set(null, (Predicate<String>) url -> false);
            Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.com/forwarded-pending"))
                    .addFlags(Intent.FLAG_ACTIVITY_FORWARD_RESULT);
            h.asApp(() -> ((ActivityTaskManagerService) h.atm).startActivity(h.thread, OWNER, null,
                    intent, null, token(child), null, -1, 0, null, null));
            check(pending.size() == 2, "Forwarding retained its stale result or removed another child's result");
            check(pending.get(0).mResultCode == Activity.RESULT_OK
                            && pending.get(1).mResultCode == Activity.RESULT_CANCELED,
                    "Pending result replacement changed the sibling's result or order");
            check(callerSnapshot.invoke(callerState, childResult.mCallerToken) == null,
                    "Forwarding retained the discarded result's caller snapshot");
            check(callerSnapshot.invoke(callerState, siblingResult.mCallerToken) != null,
                    "Forwarding removed the sibling result's caller snapshot");
        } finally { opener.set(null, previous); }
        System.out.println("External URL forwarding: original route, conflicts, ownership and pending results passed");
    }

    @SuppressWarnings("unchecked")
    private static void externalForwardedLifecycle() throws Exception {
        for (boolean accepted : new boolean[]{false, true}) {
            // Parent process dies before/during the shell call, reattaches during it,
            // loses its activity record, or rejects the result transaction.
            for (int mode = 0; mode < 5; mode++) {
                Harness h = new Harness();
                Object parent = h.record("Parent", 63, "a", mode == 4 ? "RESUMED" : "STOPPED");
                Object child = h.record("Child", 63, "a", "RESUMED", parent, "saved-route", 74);
                AppThread parentThread = new AppThread();
                parentThread.controller = h.controller;
                Object parentProcess = construct(h.processType,
                        new Class<?>[]{h.atmType, int.class, IApplicationThread.class},
                        h.atm, PID + 1, parentThread);
                set(parentProcess, "packageName", OWNER); set(parentProcess, "processName", OWNER + ":parent");
                set(parentProcess, "uid", UID); set(parentProcess, "ready", true);
                ((List<Object>) get(h.atm, "mProcesses")).add(parentProcess);
                ((List<Object>) get(h.process, "activities")).remove(parent);
                ((List<Object>) get(parentProcess, "activities")).add(parent);
                set(parent, "app", parentProcess);
                if (mode != 3) ((ActivityInfo) get(parent, "info")).flags |= ActivityInfo.FLAG_STATE_NOT_NEEDED;
                var died = h.atmType.getDeclaredMethod("processDied", h.processType);
                died.setAccessible(true);
                if (mode == 0) died.invoke(h.atm, parentProcess);
                parentThread.rejectNextResult = mode == 4;
                Field opener = field(h.atmType, "sOpenExternalUrl");
                Object previous = opener.get(null);
                final int which = mode;
                try {
                    opener.set(null, (Predicate<String>) url -> {
                        try {
                            if (which >= 1 && which <= 3) {
                                died.invoke(h.atm, parentProcess);
                                if (which == 2) {
                                    set(parent, "app", h.process);
                                    ((List<Object>) get(h.process, "activities")).add(parent);
                                }
                            }
                        } catch (Exception failure) { throw new AssertionError(failure); }
                        return accepted;
                    });
                    Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.com/saved-parent"))
                            .addFlags(Intent.FLAG_ACTIVITY_FORWARD_RESULT);
                    if (accepted) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    int[] status = {Integer.MIN_VALUE};
                    h.asApp(() -> status[0] = ((ActivityTaskManagerService) h.atm).startActivity(h.thread,
                            OWNER, null, intent, null, token(child), null, -1, 0, null, null));
                    check(status[0] == (accepted ? ActivityManager.START_SUCCESS
                                    : ActivityManager.START_INTENT_NOT_RESOLVED),
                            "Parent lifecycle changed the actual shell launch status");
                    check(get(child, "resultTo") == null, "Parent lifecycle retained the forwarded route");
                    List<ResultInfo> pending = (List<ResultInfo>) get(parent, "pendingResults");
                    check(parentThread.results(parent).isEmpty() && h.thread.results(parent).isEmpty(),
                            "Unattached or disconnected parent received an immediate result");
                    if (mode == 3) {
                        check(!h.records.contains(parent) && pending.isEmpty(),
                                "A retired parent retained a forwarded cancellation");
                        continue;
                    }
                    check(h.records.contains(parent) && pending.size() == 1,
                            "Saved parent lost its pending cancellation: mode=" + mode);
                    ResultInfo result = pending.get(0);
                    check(result.mRequestCode == 74 && "saved-route".equals(result.mResultWho)
                                    && result.mResultCode == Activity.RESULT_CANCELED
                                    && result.mData == null && result.mCallerToken == null,
                            "Saved parent cancellation lost its routing fields");
                    if (mode == 4) died.invoke(h.atm, parentProcess);
                    if (get(parent, "app") != h.process) {
                        set(parent, "app", h.process);
                        ((List<Object>) get(h.process, "activities")).add(parent);
                    }
                    // The parent becomes foreground after its already-forwarded child exits.
                    h.asApp(() -> h.controller.finishActivity(token(child),
                            Activity.RESULT_CANCELED, null, 0));
                    check(pending.isEmpty() && h.thread.results(parent).size() == 1
                                    && h.thread.results(parent).get(0) == result,
                            "Restored parent did not consume the retained cancellation exactly once");
                } finally { opener.set(null, previous); }
            }
        }
        System.out.println("External URL forwarding: process death, reattachment, retired records and failed delivery passed");
    }

    private static void externalIntentSenderResults() throws Exception {
        Harness h = new Harness();
        Object source = h.record("Caller", 52, "a", "RESUMED");
        IBinder sourceToken = token(source);
        ActivityTaskManagerService atm = (ActivityTaskManagerService) h.atm;
        IActivityManager am = (IActivityManager) get(h.atm, "mAm");
        Field opener = field(h.atmType, "sOpenExternalUrl");
        Object previousOpener = opener.get(null);
        List<String> attempts = new ArrayList<>();
        Intent web = new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.com/sender"));
        Intent mail = new Intent(Intent.ACTION_VIEW, Uri.parse("mailto:sender@example.com"));
        try {
            opener.set(null, (Predicate<String>) url -> {
                attempts.add(url);
                return url.startsWith("https:");
            });
            h.asApp(() -> {
                IIntentSender failed = am.getIntentSenderWithFeature(ActivityManager.INTENT_SENDER_ACTIVITY,
                        OWNER, null, null, null, 1, new Intent[]{mail}, new String[]{null},
                        PendingIntent.FLAG_IMMUTABLE, null, 0);
                check(atm.startActivityIntentSender(h.thread, failed, null, null, null, sourceToken,
                                "senderFailure", 21, 0, 0, null) == ActivityManager.START_INTENT_NOT_RESOLVED,
                        "Failed external IntentSender lost its launch error");
                List<ResultInfo> results = h.thread.results(source);
                check(results.size() == 1 && results.get(0).mRequestCode == 21
                                && "senderFailure".equals(results.get(0).mResultWho)
                                && results.get(0).mResultCode == Activity.RESULT_CANCELED
                                && results.get(0).mCallerToken == null && results.get(0).mData == null,
                        "IntentSender failure lost its authenticated holder's cancellation: " + results);

                IIntentSender once = am.getIntentSenderWithFeature(ActivityManager.INTENT_SENDER_ACTIVITY,
                        OWNER, null, null, null, 2, new Intent[]{web}, new String[]{null},
                        PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_ONE_SHOT, null, 0);
                check(atm.startActivityIntentSender(h.thread, once, null, null, null, sourceToken,
                                "senderNewTask", 22, Intent.FLAG_ACTIVITY_NEW_TASK,
                                Intent.FLAG_ACTIVITY_NEW_TASK, null) == ActivityManager.START_SUCCESS,
                        "Accepted one-shot external IntentSender reported failure");
                results = h.thread.results(source);
                check(results.size() == 2 && results.get(1).mRequestCode == 22
                                && "senderNewTask".equals(results.get(1).mResultWho)
                                && results.get(1).mResultCode == Activity.RESULT_CANCELED
                                && results.get(1).mCallerToken == null,
                        "NEW_TASK IntentSender lost its authenticated holder's cancellation: " + results);
                check(atm.startActivityIntentSender(h.thread, once, null, null, null, sourceToken,
                                "duplicate", 23, 0, 0, null) == ActivityManager.START_CANCELED,
                        "Consumed one-shot external IntentSender launched again");
                check(attempts.size() == 2 && h.thread.results(source).size() == 2,
                        "Consumed one-shot sent a duplicate Windows launch or activity result");

                IIntentSender ordinary = am.getIntentSenderWithFeature(ActivityManager.INTENT_SENDER_ACTIVITY,
                        OWNER, null, null, null, 3, new Intent[]{web}, new String[]{null},
                        PendingIntent.FLAG_IMMUTABLE, null, 0);
                check(atm.startActivityIntentSender(h.thread, ordinary, null, null, null, sourceToken,
                                "ordinary", 24, 0, 0, null) == ActivityManager.START_SUCCESS,
                        "Accepted ordinary IntentSender reported failure");
                check(h.thread.results(source).size() == 2,
                        "Ordinary IntentSender acceptance fabricated completion");

                set(source, "state", state("PAUSED"));
                check(atm.startActivityIntentSender(h.thread, failed, null, null, null, sourceToken,
                                "paused", 25, 0, 0, null) == ActivityManager.START_INTENT_NOT_RESOLVED,
                        "Paused IntentSender changed the launch failure");
                @SuppressWarnings("unchecked") List<ResultInfo> pending = (List<ResultInfo>) get(source, "pendingResults");
                check(h.thread.results(source).size() == 2 && pending.size() == 1
                                && pending.get(0).mRequestCode == 25 && pending.get(0).mCallerToken == null,
                        "Paused holder did not retain exactly one pending cancellation");
                set(source, "state", state("RESUMED"));

                int before = attempts.size();
                try {
                    atm.startActivityIntentSender(h.thread, failed, null, null, null, new Binder(),
                            "unknown", 26, 0, 0, null);
                    throw new AssertionError("Unknown holder token was accepted");
                } catch (SecurityException expected) {
                    check(attempts.size() == before, "Invalid holder token opened a Windows URL");
                }

                opener.set(null, (Predicate<String>) url -> {
                    attempts.add(url);
                    try { set(h.process, "exiting", true); }
                    catch (Exception failure) { throw new AssertionError(failure); }
                    return false;
                });
                check(atm.startActivityIntentSender(h.thread, failed, null, null, null, sourceToken,
                                "retired", 27, 0, 0, null) == ActivityManager.START_INTENT_NOT_RESOLVED,
                        "Exit during shell invocation changed the shell failure");
                check(h.thread.results(source).size() == 2 && pending.size() == 1,
                        "Retired IntentSender holder received a stale cancellation");
            });
        } finally {
            opener.set(null, previousOpener);
        }
        System.out.println("External IntentSender results: authenticated holder, one-shot, pending and stale results passed");
    }

    private static void externalUrlTargets() throws Exception {
        Harness h = new Harness();
        Object source = h.record("Caller", 53, "a", "RESUMED");
        IBinder sourceToken = token(source);
        ActivityTaskManagerService atm = (ActivityTaskManagerService) h.atm;
        Field opener = field(h.atmType, "sOpenExternalUrl");
        Object previousOpener = opener.get(null);
        List<String> attempts = new ArrayList<>();
        Intent web = new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.com/private"));
        try {
            opener.set(null, (Predicate<String>) url -> { attempts.add(url); return true; });
            h.asApp(() -> {
                Intent selectedMissing = new Intent(web);
                selectedMissing.setSelector(new Intent().setComponent(new ComponentName(OWNER, OWNER + ".Missing")));
                List<Intent> unresolved = List.of(
                        new Intent(web).setComponent(new ComponentName(OWNER, OWNER + ".Missing")),
                        new Intent(web).setPackage(OWNER),
                        new Intent(web).setComponent(new ComponentName(BrowserPackageSupport.PACKAGE,
                                BrowserPackageSupport.PACKAGE + ".Missing")),
                        new Intent(web).setDataAndType(web.getData(), "application/pdf"),
                        new Intent(web).addCategory("fixture.PRIVATE"),
                        new Intent(Intent.ACTION_VIEW, Uri.parse("mailto:private@example.com")).setPackage(OWNER),
                        selectedMissing);
                for (int i = 0; i < unresolved.size(); i++) {
                    Intent intent = unresolved.get(i);
                    int status = atm.startActivity(h.thread, OWNER, null, intent, intent.getType(), sourceToken,
                            "unresolved", 30 + i, 0, null, null);
                    check(status == (intent.getComponent() == null && intent.getSelector() == null
                                    ? ActivityManager.START_INTENT_NOT_RESOLVED
                                    : ActivityManager.START_CLASS_NOT_FOUND),
                            "External URL substituted an unresolved target: " + intent + " status=" + status);
                }
                check(attempts.isEmpty(), "Unresolved explicit/filter-constrained targets reached Windows");
                List<ResultInfo> results = h.thread.results(source);
                check(results.size() == unresolved.size(), "Rejected URL targets did not cancel their result requests");
                for (int i = 0; i < results.size(); i++) {
                    check(results.get(i).mRequestCode == 30 + i && results.get(i).mResultCode == Activity.RESULT_CANCELED
                                    && results.get(i).mCallerToken == null,
                            "Rejected target cancellation lost its route");
                }
                Intent selectedBrowser = new Intent(web);
                selectedBrowser.setSelector(new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.com/selection"))
                        .setPackage(BrowserPackageSupport.PACKAGE));
                for (Intent intent : List.of(web, new Intent(web).setPackage(BrowserPackageSupport.PACKAGE),
                        new Intent(web).setComponent(new ComponentName(BrowserPackageSupport.PACKAGE,
                                BrowserPackageSupport.ACTIVITY)), selectedBrowser)) {
                    check(atm.startActivity(h.thread, OWNER, null, intent, null, sourceToken,
                                    null, -1, 0, null, null) == ActivityManager.START_SUCCESS,
                            "Valid Windows browser target stopped opening");
                }
                check(attempts.equals(java.util.Collections.nCopies(4, web.getDataString())),
                        "Browser targets did not open the original URL exactly once");
            });
        } finally {
            opener.set(null, previousOpener);
        }
        System.out.println("External URL targets: explicit package/component and filter constraints passed");
    }

    @SuppressWarnings("unchecked")
    private static void externalResultOwnership() throws Exception {
        Harness h = new Harness();
        Object source = h.record("Caller", 54, "a", "RESUMED");
        Object foreign = h.record("Other", 55, "b", "RESUMED");
        AppThread otherThread = new AppThread();
        otherThread.controller = h.controller;
        Object otherProcess = construct(h.processType, new Class<?>[]{h.atmType, int.class, IApplicationThread.class},
                h.atm, PID + 1, otherThread);
        set(otherProcess, "packageName", OWNER); set(otherProcess, "processName", OWNER + ":other");
        set(otherProcess, "uid", UID); set(otherProcess, "ready", true);
        ((List<Object>) get(h.atm, "mProcesses")).add(otherProcess);
        ((List<Object>) get(h.process, "activities")).remove(foreign);
        ((List<Object>) get(otherProcess, "activities")).add(foreign);
        set(foreign, "app", otherProcess);
        ActivityTaskManagerService atm = (ActivityTaskManagerService) h.atm;
        IActivityManager am = (IActivityManager) get(h.atm, "mAm");
        Intent mail = new Intent(Intent.ACTION_VIEW, Uri.parse("mailto:ownership@example.com"));
        final IIntentSender systemSender;
        long identity = Binder.clearCallingIdentity();
        try {
            systemSender = am.getIntentSenderWithFeature(ActivityManager.INTENT_SENDER_ACTIVITY,
                    "android", null, null, null, 4, new Intent[]{mail}, new String[]{null},
                    PendingIntent.FLAG_IMMUTABLE, null, 0);
        } finally { Binder.restoreCallingIdentity(identity); }
        Field opener = field(h.atmType, "sOpenExternalUrl");
        Object previousOpener = opener.get(null);
        List<String> attempts = new ArrayList<>();
        try {
            opener.set(null, (Predicate<String>) url -> { attempts.add(url); return false; });
            h.asApp(() -> {
                check(atm.startActivityIntentSender(h.thread, systemSender, null, null, null, token(source),
                                "systemSender", 41, 0, 0, null) == ActivityManager.START_INTENT_NOT_RESOLVED,
                        "System-created URL IntentSender lost its failure");
                check(h.thread.results(source).size() == 1 && h.thread.results(source).get(0).mRequestCode == 41,
                        "System-created IntentSender cancellation did not reach the holder");
                try {
                    atm.startActivityIntentSender(h.thread, systemSender, null, null, null, token(foreign),
                            "foreign", 42, 0, 0, null);
                    throw new AssertionError("Foreign-process result activity was accepted");
                } catch (SecurityException expected) {
                    check(attempts.size() == 1, "Foreign result token opened a Windows URL");
                }
                for (IBinder token : List.of(token(foreign), new Binder())) {
                    check(atm.startActivity(h.thread, OWNER, null, mail, null, token,
                                    "unowned", 43, 0, null, null) == ActivityManager.START_INTENT_NOT_RESOLVED,
                            "Unowned result token changed launch failure");
                }
                check(h.thread.results(source).size() == 1 && otherThread.results(foreign).isEmpty(),
                        "Launch cancellation was injected into an unowned result activity");
                opener.set(null, (Predicate<String>) url -> {
                    try { set(source, "app", otherProcess); }
                    catch (Exception failure) { throw new AssertionError(failure); }
                    return false;
                });
                check(atm.startActivityIntentSender(h.thread, systemSender, null, null, null, token(source),
                                "reattached", 44, 0, 0, null) == ActivityManager.START_INTENT_NOT_RESOLVED,
                        "Recipient reattachment changed launch failure");
                check(h.thread.results(source).size() == 1 && otherThread.results(source).isEmpty(),
                        "Result crossed to a process attached while the shell opener ran");
                set(source, "app", h.process);
                opener.set(null, (Predicate<String>) url -> false);
                java.io.PrintStream previousOut = System.out;
                boolean[] reattached = {false};
                // Force reattachment after result admission but before the Binder
                // send. Logging is an isolated test boundary, not a production hook.
                System.setOut(new java.io.PrintStream(previousOut, true) {
                    @Override public void println(String line) {
                        previousOut.println(line);
                        if (!reattached[0] && line.startsWith("system_server: scheduleTransaction ActivityResultItem to ")) {
                            reattached[0] = true;
                            try { set(source, "app", otherProcess); }
                            catch (Exception failure) { throw new AssertionError(failure); }
                        }
                    }
                });
                try {
                    check(atm.startActivityIntentSender(h.thread, systemSender, null, null, null, token(source),
                                    "dispatchRace", 45, 0, 0, null) == ActivityManager.START_INTENT_NOT_RESOLVED,
                            "Reattachment during result dispatch changed launch failure");
                } finally { System.setOut(previousOut); }
                check(reattached[0] && h.thread.results(source).size() == 2
                                && h.thread.results(source).get(1).mRequestCode == 45
                                && otherThread.results(source).isEmpty(),
                        "Admitted cancellation was redirected to a replacement application thread");
            });
        } finally {
            opener.set(null, previousOpener);
        }
        System.out.println("External URL ownership: creator/holder separation and foreign or reattached recipients passed");
    }

    public static int run(boolean realBinder, String group) throws Exception {
        binder = realBinder; checks = 0;
        if (group.equals("all") || group.equals("task")) removeTask();
        if (group.equals("all") || group.equals("affinity")) affinity();
        if (group.equals("all") || group.equals("sub")) subActivities();
        if (group.equals("all")) externalUrlResults();
        if (group.equals("all")) externalForwardedResults();
        if (group.equals("all")) externalForwardedLifecycle();
        if (group.equals("all")) externalIntentSenderResults();
        if (group.equals("all")) externalUrlTargets();
        if (group.equals("all")) externalResultOwnership();
        return checks;
    }
}
