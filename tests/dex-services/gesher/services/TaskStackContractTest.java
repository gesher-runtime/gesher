/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.app.ActivityThread;
import android.app.IActivityManager;
import android.app.PendingIntent;
import android.app.TaskStackBuilder;
import android.app.IActivityTaskManager;
import android.app.IApplicationThread;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.ActivityTransactionItem;
import android.app.servertransaction.LaunchActivityItem;
import android.app.servertransaction.PauseActivityItem;
import android.app.servertransaction.ResumeActivityItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.ActivityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import android.view.Display;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.StandIn;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import sun.misc.Unsafe;

/**
 * Exercises production startActivities, task snapshots and lifecycle handoffs.
 * No Android ActivityThread is simulated: test callbacks explicitly acknowledge
 * the real service's queued transitions in controlled order.
 */
public final class TaskStackContractTest {
    private static final String PKG = "fixture.taskstack";
    private static final int PID = 884370, UID = 12730;
    private static int checks;
    private static boolean realBinder = true;
    private static final Unsafe UNSAFE;
    static {
        try {
            Field f = Unsafe.class.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            UNSAFE = (Unsafe) f.get(null);
        } catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Checked { void run() throws Exception; }
    private static void rejects(Class<? extends Throwable> expected, Checked action) throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            if (expected.isInstance(failure)) { checks++; return; }
            throw new AssertionError("Expected " + expected.getSimpleName() + ", got " + failure, failure);
        }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }
    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }
    private static Class<?> type(String name) throws Exception {
        return Class.forName("com.gesher.server." + name);
    }
    private static Field field(Class<?> clazz, String name) throws Exception {
        Field field = clazz.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }
    private static Object get(Object target, String name) throws Exception {
        return field(target.getClass(), name).get(target);
    }
    private static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }
    private static Object make(Class<?> clazz, Class<?>[] signature, Object... args) throws Exception {
        Constructor<?> ctor = clazz.getDeclaredConstructor(signature);
        ctor.setAccessible(true);
        return ctor.newInstance(args);
    }
    private static Intent launch(String name) {
        return launch(PKG, name);
    }
    private static Intent launch(String pkg, String name) {
        return new Intent(pkg + "." + name).setComponent(new ComponentName(pkg, pkg + "." + name));
    }
    private static ApkPackage code(String pkg, int uid) throws Exception {
        String path = "/fixture/" + pkg + "/base.apk";
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(pkg, path);
        parsed.setEnabled(true);
        parsed.setTargetSdkVersion(36);
        for (String name : List.of("Root", "Middle", "Top", "Other", "Private", "Guarded", "Single", "Doc")) {
            ParsedActivityImpl activity = new ParsedActivityImpl();
            activity.setPackageName(pkg);
            activity.setName(pkg + "." + name);
            activity.setTaskAffinity(pkg);
            activity.setEnabled(true);
            activity.setExported(!name.equals("Private"));
            if (name.equals("Guarded")) activity.setPermission("fixture.permission.NOT_GRANTED");
            if (name.equals("Single")) activity.setLaunchMode(ActivityInfo.LAUNCH_SINGLE_TASK);
            if (name.equals("Doc")) activity.setDocumentLaunchMode(ActivityInfo.DOCUMENT_LAUNCH_INTO_EXISTING);
            parsed.addActivity(activity);
        }
        var provider = new com.android.internal.pm.pkg.component.ParsedProviderImpl();
        provider.setName(pkg + ".GrantProvider"); provider.setPackageName(pkg);
        provider.setAuthority(pkg + ".grants"); provider.setEnabled(true);
        provider.setExported(false); provider.setGrantUriPermissions(true);
        provider.setReadPermission(pkg + ".READ_PRIVATE"); parsed.addProvider(provider);
        parsed.hideAsFinal();
        ApkPackage apk = (ApkPackage) UNSAFE.allocateInstance(ApkPackage.class);
        set(apk, "packageName", pkg);
        set(apk, "parsed", parsed);
        set(apk, "mUid", uid);
        set(apk, "apkPath", path);
        set(apk, "mDataDir", "/fixture/" + pkg + "/data");
        set(apk, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        // Constructed package-manager metadata only; signing verification has its
        // own APK fixtures. The real capability checks still compare this identity.
        set(apk, "mSigningIdentityChecked", true);
        set(apk, "mVerifiedSignatures", new android.content.pm.Signature[]{
                new android.content.pm.Signature(new byte[]{(byte)uid})});
        return apk;
    }
    private static final class AppThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        final List<ClientTransaction> transactions = new ArrayList<>();
        Runnable onSchedule;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleTransaction(ClientTransaction t) {
            transactions.add(t);
            if (onSchedule != null) onSchedule.run();
        }
        int launches(Object record) throws Exception {
            return items(LaunchActivityItem.class, record);
        }
        int items(Class<?> kind, Object record) throws Exception {
            IBinder token = record == null ? null : (IBinder)get(record, "token");
            int count = 0;
            for (ClientTransaction transaction : transactions) {
                for (var item : transaction.getTransactionItems()) {
                    IBinder target = item instanceof LaunchActivityItem launch ? launch.getActivityToken()
                            : item instanceof ActivityTransactionItem activity ? activity.getActivityToken() : null;
                    if (kind.isInstance(item) && (token == null || target == token)) count++;
                }
            }
            return count;
        }
    }
    private static final class Harness {
        final Object tasks;
        final Object packages;
        final ApkPackage owner;
        final IActivityManager am;
        final Object process;
        final IActivityTaskManager api;
        final AppThread thread = new AppThread();
        final Class<?> record;
        @SuppressWarnings("unchecked")
        Harness() throws Exception {
            Class<?> pm = type("PackageManagerService"), atm = type("ActivityTaskManagerService");
            record = type("ActivityTaskManagerService$ActivityRecord");
            owner = code(PKG, UID);
            packages = make(pm, new Class<?>[]{Collection.class}, List.of(owner,
                    code(PKG + ".second", UID + 1), code(PKG + ".third", UID + 2)));
            tasks = make(atm, new Class<?>[]{pm}, packages);
            am = (IActivityManager) make(type("ActivityManagerService"),
                    new Class<?>[]{atm, pm, type("NotificationManagerService")},
                    tasks, packages, null);
            Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
            long original = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)UID << 32) | PID);
                process = make(processType, new Class<?>[]{atm, int.class, IApplicationThread.class},
                        tasks, PID, thread);
            } finally { Binder.restoreCallingIdentity(original); }
            set(process, "packageName", PKG);
            set(process, "processName", PKG);
            set(process, "ready", true);
            ((List<Object>)get(tasks, "mProcesses")).add(process);
            if (!realBinder) { api = (IActivityTaskManager) tasks; return; }
            IBinder target = StandIn.of(IActivityTaskManager.class,
                    (IActivityTaskManager) tasks).asBinder();
            IBinder transport = new Binder() {
                @Override public IInterface queryLocalInterface(String descriptor) { return null; }
                @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                        throws RemoteException {
                    return target.transact(code, request, reply, flags);
                }
            };
            api = IActivityTaskManager.Stub.asInterface(transport);
            check(api != tasks && api.asBinder() == transport,
                    "TaskStack calls did not enter the generated AIDL Parcel transport");
        }
        void asApp(Checked action) throws Exception {
            long previous = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)UID << 32) | PID);
                action.run();
                check(Binder.getCallingPid() == PID && Binder.getCallingUid() == UID,
                        "Caller identity was not preserved");
            } finally { Binder.restoreCallingIdentity(previous); }
        }
        @SuppressWarnings("unchecked")
        List<Object> records() throws Exception {
            return (List<Object>) get(tasks, "mActivities");
        }
        Object record(String name) throws Exception {
            return record(PKG, name);
        }
        Object record(String pkg, String name) throws Exception {
            for (Object record : records()) {
                ActivityInfo info = (ActivityInfo) get(record, "info");
                if ((pkg + "." + name).equals(info.name) && !(Boolean)get(record, "finishing")) return record;
            }
            throw new AssertionError("No record for " + name);
        }
        String state(Object activity) throws Exception {
            return get(activity, "state").toString();
        }
        void acknowledge(String method, Object activity) throws Exception {
            Method callback = tasks.getClass().getDeclaredMethod(method, record);
            callback.setAccessible(true);
            callback.invoke(tasks, activity);
        }
        int start(Intent... intents) throws Exception {
            final int[] status = new int[1];
            asApp(() -> status[0] = api.startActivities(thread, PKG, null, intents, new String[intents.length],
                    null, null, 0));
            return status[0];
        }
        void finish(Object activity) throws Exception {
            asApp(() -> check(api.getActivityClientController().finishActivity(
                    (IBinder)get(activity, "token"), 0, null, 0), "Public finish rejected its owner"));
            acknowledge("activityPaused", activity);
        }
        Context context() {
            return new ContextWrapper(null) {
                @Override public String getPackageName() { return PKG; }
                @Override public String getOpPackageName() { return PKG; }
                @Override public String getAttributionTag() { return null; }
                @Override public int getUserId() { return 0; }
                @Override public int getDisplayId() { return Display.DEFAULT_DISPLAY; }
                @Override public android.os.UserHandle getUser() { return android.os.UserHandle.SYSTEM; }
                @Override public ApplicationInfo getApplicationInfo() { return owner.applicationInfo(0); }
                @Override public ContentResolver getContentResolver() { return null; }
                @Override public void startActivities(Intent[] intents, Bundle options) {
                    try {
                        int status = api.startActivities(thread, PKG, null, intents,
                                new String[intents.length], null, options, 0);
                        if (status < 0) throw new AssertionError("Public stack rejected: " + status);
                    } catch (RemoteException failure) { throw new AssertionError(failure); }
                }
            };
        }
    }
    private static void fullStack() throws Exception {
        Harness h = new Harness();
        check(h.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                launch("Middle"), launch("Top")) == ActivityManager.START_SUCCESS, "Stack rejected");
        Object root = h.record("Root"), middle = h.record("Middle"), top = h.record("Top");
        check(h.records().size() == 3 && get(root, "taskId").equals(get(middle, "taskId"))
                && get(root, "taskId").equals(get(top, "taskId")), "History lost or split");
        check("INITIALIZING".equals(h.state(root)) && "INITIALIZING".equals(h.state(middle))
                && get(root, "app") == null && get(middle, "app") == null,
                "Opaque parent history was eagerly instantiated or attached");
        check("LAUNCHING".equals(h.state(top)) && h.thread.launches(null) == 1
                && h.thread.launches(top) == 1, "Only the final opaque activity should launch");
        List<ActivityManager.RunningTaskInfo> tasks = h.api.getTasks(1, false, true, Display.DEFAULT_DISPLAY);
        check(tasks.size() == 1 && tasks.get(0).numActivities == 3
                && (PKG + ".Root").equals(tasks.get(0).baseActivity.getClassName())
                && (PKG + ".Top").equals(tasks.get(0).topActivity.getClassName()), "Wrong public task snapshot");
        h.acknowledge("activityResumed", top); h.finish(top);
        check("LAUNCHING".equals(h.state(middle)) && h.thread.launches(middle) == 1
                && h.thread.launches(root) == 0, "Back did not realize only the next history entry");
        h.acknowledge("activityResumed", middle); h.finish(middle);
        check("LAUNCHING".equals(h.state(root)) && h.thread.launches(root) == 1,
                "Second Back did not realize root exactly once");
        h.acknowledge("activityPaused", middle);
        check(h.thread.launches(root) == 1, "Duplicate pause acknowledgement relaunched root");
    }

    private static int startFromCreator(Harness h, String creator,
            java.util.function.BooleanSupplier lifetime, Intent... intents) throws Exception {
        Method start = h.tasks.getClass().getDeclaredMethod("startActivitiesForVerifiedCreator",
                String.class, Intent[].class, String[].class,
                java.util.function.BooleanSupplier.class, Bundle.class);
        start.setAccessible(true);
        return (Integer)start.invoke(h.tasks, creator, intents, new String[intents.length], lifetime, null);
    }

    /** Package publication boundary only; this fixture does not run an APK installer/cache. */
    @SuppressWarnings("unchecked")
    private static void publishFixtureCode(Harness h, String name, ApkPackage replacement) throws Exception {
        java.util.Map<String, ApkPackage> updated = new java.util.LinkedHashMap<>(
                (java.util.Map<String, ApkPackage>)get(h.packages, "mPackages"));
        if (replacement == null) updated.remove(name); else updated.put(name, replacement);
        set(h.packages, "mPackages", updated);
    }

    private static void acceptedHistoryLifetimes() throws Exception {
        String creator = PKG + ".second";
        for (boolean removeCreator : new boolean[]{false, true}) {
            Harness h = new Harness();
            check(startFromCreator(h, creator, () -> true,
                    launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"), launch("Top"))
                    == ActivityManager.START_SUCCESS, "Cross-package creator stack rejected");
            Object root = h.record("Root"), middle = h.record("Middle"), top = h.record("Top");
            check(h.records().size() == 3 && get(root, "taskId").equals(get(top, "taskId"))
                    && get(middle, "taskId").equals(get(top, "taskId")), "Creator history split");
            check(h.thread.launches(top) == 1 && h.thread.launches(middle) == 0,
                    "Creator history did not defer its opaque parents");
            h.acknowledge("activityResumed", top);
            publishFixtureCode(h, creator, removeCreator ? null : code(creator, UID + 1));
            h.finish(top);
            check(h.records().contains(middle) && "LAUNCHING".equals(h.state(middle))
                    && h.thread.launches(middle) == 1 && h.thread.launches(root) == 0,
                    "Accepted target history was erased after creator " + (removeCreator ? "removal" : "update"));
            h.acknowledge("activityResumed", middle); h.finish(middle);
            check("LAUNCHING".equals(h.state(root)) && h.thread.launches(root) == 1,
                    "Second Back lost accepted root after creator retirement");
        }

        Harness retiredTarget = new Harness();
        startFromCreator(retiredTarget, creator, () -> true,
                launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"), launch("Top"));
        Object root = retiredTarget.record("Root"), middle = retiredTarget.record("Middle"),
                top = retiredTarget.record("Top");
        retiredTarget.acknowledge("activityResumed", top);
        publishFixtureCode(retiredTarget, PKG, code(PKG, UID));
        Method cleanup = retiredTarget.tasks.getClass().getDeclaredMethod("finish",
                retiredTarget.record, String.class, int.class, Intent.class);
        cleanup.setAccessible(true);
        cleanup.invoke(retiredTarget.tasks, top, "target-update", 0, null);
        retiredTarget.acknowledge("activityPaused", top);
        check(!retiredTarget.records().contains(root) && !retiredTarget.records().contains(middle)
                && retiredTarget.thread.launches(root) == 0 && retiredTarget.thread.launches(middle) == 0,
                "Creator lease release allowed retired target code to launch");

        Harness beforePublication = new Harness();
        java.util.concurrent.atomic.AtomicBoolean admitted = new java.util.concurrent.atomic.AtomicBoolean(true);
        set(beforePublication.tasks, "mBeforeActivityPublication", (Runnable)() -> admitted.set(false));
        check(startFromCreator(beforePublication, creator, admitted::get,
                launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Top"))
                == ActivityManager.START_CANCELED && beforePublication.records().isEmpty(),
                "History bypassed creator admission before publication");

        Harness ordinary = new Harness();
        set(ordinary.process, "ready", false);
        startFromCreator(ordinary, creator, () -> true, launch("Top").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        check(ordinary.thread.launches(null) == 0 && ordinary.records().size() == 1,
                "Ordinary asynchronous start did not wait for attachment");
        publishFixtureCode(ordinary, creator, code(creator, UID + 1));
        Method attached = ordinary.tasks.getClass().getDeclaredMethod("finishAttachApplication", int.class);
        attached.setAccessible(true); attached.invoke(ordinary.tasks, PID);
        check(ordinary.thread.launches(null) == 0 && ordinary.records().isEmpty(),
                "History lease release changed ordinary async launch admission");
    }

    /** Disabled deferred entries must be removed without creating them or disturbing the top. */
    @SuppressWarnings("unchecked")
    private static void disabledDeferredHistory() throws Exception {
        Harness h = new Harness();
        h.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"), launch("Top"));
        Object root = h.record("Root"), middle = h.record("Middle"), top = h.record("Top");
        Method enabled = ApkPackage.class.getDeclaredMethod("setComponentEnabledState", String.class, int.class);
        enabled.setAccessible(true);
        enabled.invoke(h.owner, PKG + ".Middle", 2);
        Class<?> changeType = type("PackageManagerService$EnabledChange");
        Object change = make(changeType, new Class<?>[]{ApkPackage.class}, h.owner);
        ((List<String>)get(change, "components")).add(PKG + ".Middle");
        Method cleanup = h.tasks.getClass().getDeclaredMethod("cleanupDisabledPackageComponents", changeType);
        cleanup.setAccessible(true); cleanup.invoke(h.tasks, change);
        check(!h.records().contains(middle) && h.records().contains(root) && h.records().contains(top),
                "Disabled deferred parent retained or its live siblings were removed");
        check(h.thread.launches(middle) == 0 && h.thread.launches(root) == 0,
                "Disabled parent cleanup eagerly instantiated history");
        enabled.invoke(h.owner, PKG + ".Middle", 1);
        check(!h.records().contains(middle), "Enabling revived a previously removed history entry");
        h.acknowledge("activityResumed", top); h.finish(top);
        check(h.thread.launches(root) == 1 && h.thread.launches(middle) == 0,
                "Back did not skip the disabled/removed deferred parent");
    }

    private static void invalidBatch() throws Exception {
        Harness h = new Harness();
        Intent root = launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        rejects(NullPointerException.class, () -> h.asApp(() -> h.api.startActivities(h.thread,
                PKG, null, null, new String[0], null, null, 0)));
        rejects(NullPointerException.class, () -> h.asApp(() -> h.api.startActivities(h.thread,
                PKG, null, new Intent[]{root}, null, null, null, 0)));
        rejects(IllegalArgumentException.class, () -> h.asApp(() -> h.api.startActivities(h.thread,
                PKG, null, new Intent[]{root, launch("Top")}, new String[1], null, null, 0)));
        check(h.records().isEmpty() && h.thread.transactions.isEmpty(), "Invalid input mutated history");
        check(h.start(root, null, launch("Top")) == ActivityManager.START_SUCCESS
                && h.records().size() == 2, "AOSP null-member filtering changed");
        Harness missing = new Harness();
        check(missing.start(launch("DoesNotExist")) == ActivityManager.START_INTENT_NOT_RESOLVED
                && missing.records().isEmpty(), "Unresolved target reported success");
        missing.start(root, launch("DoesNotExist"));
        // AOSP documents undefined partial history after an unresolved member.
        missing.start(launch("Other").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK));
        check(get(missing.record("Other"), "batchOwner") == null
                && "LAUNCHING".equals(missing.state(missing.record("Other"))), "Failure leaked publication scope");
    }

    private static void taskFlags() throws Exception {
        Harness h = new Harness();
        h.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                launch("Other").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK),
                launch("Top"));
        Object root = h.record("Root"), other = h.record("Other"), top = h.record("Top");
        check(!get(root, "taskId").equals(get(other, "taskId")) && get(other, "taskId").equals(get(top, "taskId")),
                "Later NEW_TASK/MULTIPLE_TASK was forced into the first task");
        check(h.thread.launches(root) == 1 && h.thread.launches(other) == 0 && h.thread.launches(top) == 1,
                "Separate tasks did not realize only their visible tops");
        Harness cross = new Harness();
        cross.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                launch(PKG + ".second", "Root"), launch(PKG + ".third", "Top"));
        Object a = cross.record("Root"), b = cross.record(PKG + ".second", "Root"),
                c = cross.record(PKG + ".third", "Top");
        check(get(a, "taskId").equals(get(b, "taskId")) && !get(b, "taskId").equals(get(c, "taskId")),
                "Different-UID predecessor became the next source");
        check((((Intent)get(c, "intent")).getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0,
                "Different-UID successor lost AOSP NEW_TASK");
        Harness clear = new Harness();
        clear.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Top"));
        Object oldRoot = clear.record("Root"), oldTop = clear.record("Top");
        clear.acknowledge("activityResumed", oldTop);
        int task = (Integer)get(oldRoot, "taskId");
        clear.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK),
                launch("Middle"));
        check(!clear.records().contains(oldRoot) && (Boolean)get(oldTop, "finishing")
                && (Integer)get(clear.record("Root"), "taskId") == task, "CLEAR_TASK retained old history");
        clear.acknowledge("activityPaused", oldTop);
        check("LAUNCHING".equals(clear.state(clear.record("Middle")))
                && clear.thread.launches(clear.record("Root")) == 0, "Replacement eagerly launched root");
        for (boolean singleTop : new boolean[]{false, true}) {
            Harness same = new Harness();
            same.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"),
                    launch("Root").addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP
                            | (singleTop ? Intent.FLAG_ACTIVITY_SINGLE_TOP : 0)));
            check(same.records().size() == 1 && same.thread.launches(same.record("Root")) == 1
                    && same.thread.launches(null) == 1, "CLEAR_TOP failed to replace/reuse deferred history");
        }
        Harness reuse = new Harness();
        reuse.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Top"));
        Object retainedRoot = reuse.record("Root"), retainedTop = reuse.record("Top");
        reuse.acknowledge("activityResumed", retainedTop);
        reuse.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        check(reuse.records().size() == 2 && reuse.record("Root") == retainedRoot
                && reuse.record("Top") == retainedTop && reuse.thread.launches(null) == 1,
                "NEW_TASK duplicated its existing matching task root");
        Harness reorder = new Harness();
        reorder.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"),
                launch("Root").addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
        check(reorder.records().size() == 2 && reorder.records().get(1) == reorder.record("Root")
                && reorder.thread.launches(null) == 1 && reorder.thread.launches(reorder.record("Root")) == 1,
                "REORDER_TO_FRONT duplicated/instantiated the wrong history entry");
        Harness document = new Harness();
        Intent doc = launch("Doc").setData(android.net.Uri.parse("content://fixture/document/1"));
        document.start(doc, launch("Top"));
        Object docRoot = document.record("Doc"), docTop = document.record("Top");
        document.acknowledge("activityResumed", docTop);
        document.start(doc);
        check(document.record("Doc") == docRoot && (Boolean)get(docTop, "finishing"),
                "document intoExisting did not clear to its existing root");
        document.acknowledge("activityPaused", docTop);
        check(document.thread.launches(docRoot) == 1, "Document root was not realized after foreground pause");
        document.start(new Intent(doc).setData(android.net.Uri.parse("content://fixture/document/2")));
        check(document.api.getTasks(10, false, true, Display.DEFAULT_DISPLAY).size() == 2,
                "Different document data reused the previous document task");
        Harness single = new Harness();
        single.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Single"), launch("Top"));
        Object unique = single.record("Single"), above = single.record("Top");
        single.acknowledge("activityResumed", above);
        single.start(launch("Single"));
        check(single.record("Single") == unique && (Boolean)get(above, "finishing"),
                "singleTask failed to reuse its existing task/instance");
        single.acknowledge("activityPaused", above);
        check(single.thread.launches(unique) == 1 && single.thread.launches(single.record("Root")) == 0,
                "singleTask realized unrelated parent history");
        Harness options = new Harness();
        options.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        Object optionRoot = options.record("Root");
        options.acknowledge("activityResumed", optionRoot);
        int requestedTask = (Integer)get(optionRoot, "taskId");
        ActivityOptions launchOptions = ActivityOptions.makeBasic().setShareIdentityEnabled(true);
        launchOptions.setLaunchTaskId(requestedTask);
        rejects(SecurityException.class, () -> options.asApp(() -> options.api.startActivities(options.thread, PKG, null,
                new Intent[]{launch("Other").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK), launch("Top")},
                new String[2], null, launchOptions.toBundle(), 0)));
        check(options.records().size() == 1, "Unauthorized task options published a stack prefix");
        long systemIdentity = Binder.clearCallingIdentity();
        try {
            options.api.startActivities(null, "android", null,
                    new Intent[]{launch("Other").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK), launch("Top")},
                    new String[2], null, launchOptions.toBundle(), 0);
        } finally { Binder.restoreCallingIdentity(systemIdentity); }
        Object optionParent = options.record("Other"), optionTop = options.record("Top");
        check((Integer)get(optionParent, "taskId") != requestedTask
                && (Integer)get(optionTop, "taskId") == requestedTask,
                "Top-only launchTaskId was applied to every stack member");
        java.util.Map<?, ?> parentCallers = (java.util.Map<?, ?>)get(get(optionParent, "callerState"), "callers");
        java.util.Map<?, ?> topCallers = (java.util.Map<?, ?>)get(get(optionTop, "callerState"), "callers");
        check(!(Boolean)get(parentCallers.values().iterator().next(), "shareIdentity")
                && (Boolean)get(topCallers.values().iterator().next(), "shareIdentity"),
                "Top-only identity sharing disclosed the source to parent history");
    }

    private static void visibilityAndRemoval() throws Exception {
        Harness h = new Harness();
        h.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"), launch("Top"));
        Object root = h.record("Root"), middle = h.record("Middle"), top = h.record("Top");
        h.acknowledge("activityResumed", top);
        h.asApp(() -> check(h.api.getActivityClientController().convertToTranslucent(
                (IBinder)get(top, "token"), null), "Translucent conversion rejected"));
        check(h.thread.launches(middle) == 1 && h.thread.launches(root) == 0
                && h.thread.items(PauseActivityItem.class, middle) == 1
                && h.thread.items(ResumeActivityItem.class, middle) == 0,
                "Newly visible history should be created paused beneath the translucent top");
        h.acknowledge("activityPaused", middle);
        h.finish(top);
        check(h.thread.launches(middle) == 1 && h.thread.items(ResumeActivityItem.class, middle) == 1,
                "Back recreated/resumed a visible parent more than once");
        Harness removal = new Harness();
        removal.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"), launch("Top"));
        Object removedTop = removal.record("Top");
        removal.acknowledge("activityResumed", removedTop);
        final boolean[] paused = {false};
        removal.thread.onSchedule = () -> {
            if (paused[0]) return;
            try {
                if (!"PAUSING".equals(removal.state(removedTop))) return;
                paused[0] = true;
                removal.acknowledge("activityPaused", removedTop);
            } catch (Exception failure) { throw new AssertionError(failure); }
        };
        check(removal.api.removeTask((Integer)get(removedTop, "taskId")), "Task removal rejected");
        check(paused[0] && removal.thread.launches(null) == 1
                && removal.records().stream().allMatch(record -> {
                    try { return (Boolean)get(record, "finishing"); }
                    catch (Exception failure) { throw new AssertionError(failure); }
                }), "Task removal instantiated history during synchronous pause reentry");
    }

    private static void attachAndReentry() throws Exception {
        Harness h = new Harness();
        set(h.process, "ready", false);
        h.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"), launch("Top"));
        check(h.thread.transactions.isEmpty() && get(h.record("Root"), "app") == null, "Cold parents launched");
        Method attached = h.tasks.getClass().getDeclaredMethod("finishAttachApplication", int.class);
        attached.setAccessible(true); attached.invoke(h.tasks, PID);
        check(h.thread.launches(null) == 1 && h.thread.launches(h.record("Top")) == 1,
                "Attachment instantiated deferred parents");
        Harness reentrant = new Harness();
        final boolean[] called = {false};
        reentrant.thread.onSchedule = () -> {
            if (called[0]) return;
            called[0] = true;
            try {
                check(reentrant.records().size() == 3 && reentrant.thread.launches(reentrant.record("Root")) == 0,
                        "Callback observed an incomplete/eager stack");
                reentrant.acknowledge("activityResumed", reentrant.record("Top"));
                reentrant.start(launch("Other").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK));
            } catch (Exception failure) { throw new AssertionError(failure); }
        };
        reentrant.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"), launch("Top"));
        check(called[0] && "RESUMED".equals(reentrant.state(reentrant.record("Top"))), "Synchronous callback corrupted stack");
        check(!get(reentrant.record("Other"), "taskId").equals(get(reentrant.record("Root"), "taskId"))
                && reentrant.thread.launches(reentrant.record("Other")) == 1,
                "A reentrant launch inherited another batch's task/publication scope");
        Harness many = new Harness();
        Intent[] stack = new Intent[64];
        for (int i = 0; i < stack.length; i++) stack[i] = launch(i == stack.length - 1 ? "Top" : "Root");
        stack[0].addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); many.start(stack);
        check(many.records().size() == stack.length && many.thread.launches(null) == 1
                && ((List<?>)get(many.process, "activities")).size() == 1,
                "Deep history allocated one live Activity per parent");
        Harness death = new Harness();
        death.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"), launch("Top"));
        Object next = death.record("Middle"), bottom = death.record("Root");
        Method died = death.tasks.getClass().getDeclaredMethod("processDied", death.process.getClass());
        died.setAccessible(true); died.invoke(death.tasks, death.process);
        check(death.records().size() == 2 && !(Boolean)get(next, "deferredHistory")
                && get(next, "app") == null && (Boolean)get(bottom, "deferredHistory"),
                "Foreground process death stranded or eagerly instantiated the retained history");
        Harness failure = new Harness();
        failure.start(launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"), launch("Top"));
        Class<?> starter = type("ActivityTaskManagerService$ProcessStarter");
        java.util.concurrent.atomic.AtomicInteger starts = new java.util.concurrent.atomic.AtomicInteger();
        set(failure.tasks, "mStarter", java.lang.reflect.Proxy.newProxyInstance(starter.getClassLoader(),
                new Class<?>[]{starter}, (proxy, method, args) -> { starts.incrementAndGet(); return 0; }));
        died.invoke(failure.tasks, failure.process);
        check(starts.get() == 2 && failure.records().isEmpty()
                && ((java.util.Map<?, ?>)get(failure.tasks, "mStarting")).isEmpty()
                && ((List<?>)get(failure.tasks, "mTaskOrder")).isEmpty(),
                "Failed history activation retained records/tasks or retried the same activity");
    }

    private static void permissions() throws Exception {
        Harness h = new Harness();
        rejects(SecurityException.class, () -> h.start(launch(PKG + ".second", "Private")));
        check(h.records().isEmpty(), "Unexported target was published");
        rejects(SecurityException.class, () -> h.start(launch(PKG + ".second", "Guarded")));
        check(h.records().isEmpty(), "Permission-protected target was published without its permission");
        rejects(SecurityException.class, () -> h.asApp(() -> h.api.startActivities(h.thread,
                PKG + ".second", null, new Intent[]{launch("Root")}, new String[1], null, null, 0)));
        check(h.records().isEmpty(), "Caller package spoofing published history");
    }

    private static void pendingStacks() throws Exception {
        Harness h = new Harness();
        Object singleton = field(ActivityManager.class, "IActivityManagerSingleton").get(null);
        Field instance = field(android.util.Singleton.class, "mInstance");
        Object previous = instance.get(singleton);
        Field current = field(ActivityThread.class, "sCurrentActivityThread");
        Object previousThread = current.get(null);
        try {
            ActivityThread frameworkThread = (ActivityThread)UNSAFE.allocateInstance(ActivityThread.class);
            Class<?> appThreadType = Class.forName("android.app.ActivityThread$ApplicationThread");
            set(frameworkThread, "mAppThread", make(appThreadType, new Class<?>[]{ActivityThread.class}, frameworkThread));
            current.set(null, frameworkThread);
            IActivityManager manager = h.am;
            if (realBinder) {
                IBinder target = StandIn.of(IActivityManager.class, h.am).asBinder();
                manager = IActivityManager.Stub.asInterface(new Binder() {
                    @Override public IInterface queryLocalInterface(String descriptor) { return null; }
                    @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                            throws RemoteException { return target.transact(code, request, reply, flags); }
                });
            }
            instance.set(singleton, manager);
            final PendingIntent[] pending = new PendingIntent[1];
            h.asApp(() -> pending[0] = TaskStackBuilder.create(h.context())
                    .addNextIntent(launch("Root")).addNextIntent(launch("Middle")).addNextIntent(launch("Top"))
                    .getPendingIntent(601, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_ONE_SHOT));
            h.asApp(() -> pending[0].send());
            check(h.records().size() == 3 && h.thread.launches(null) == 1
                    && h.thread.launches(h.record("Top")) == 1, "Public PendingIntent lost/developed eager parents");
            check(PKG.equals(get(h.record("Root"), "launchedFromPackage"))
                    && PKG.equals(get(h.record("Top"), "launchedFromPackage")), "Pending stack lost creator");
            rejects(PendingIntent.CanceledException.class, () -> h.asApp(() -> pending[0].send()));
            check(h.records().size() == 3, "One-shot token created another stack");
            final PendingIntent[] denied = new PendingIntent[1];
            h.asApp(() -> denied[0] = PendingIntent.getActivities(h.context(), 602,
                    new Intent[]{launch(PKG + ".second", "Private").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Other")},
                    PendingIntent.FLAG_IMMUTABLE));
            h.asApp(() -> denied[0].send());
            check(h.records().size() == 3 && h.thread.launches(null) == 1,
                    "Pending activity stack skipped a denied parent or granted holder/system authority");
            ActivityOptions taskOptions = ActivityOptions.makeBasic();
            taskOptions.setLaunchTaskId((Integer)get(h.record("Top"), "taskId"));
            Intent[] optionStack = {launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK), launch("Other")};
            h.asApp(() -> denied[0] = PendingIntent.getActivities(h.context(), 603,
                    optionStack, PendingIntent.FLAG_IMMUTABLE, taskOptions.toBundle()));
            long systemIdentity = Binder.clearCallingIdentity();
            try { denied[0].send(); } finally { Binder.restoreCallingIdentity(systemIdentity); }
            check(h.records().size() == 3, "System sender elevated unprivileged creator task options");
            systemIdentity = Binder.clearCallingIdentity();
            try { denied[0] = PendingIntent.getActivities(h.context(), 604,
                    optionStack, PendingIntent.FLAG_IMMUTABLE); }
            finally { Binder.restoreCallingIdentity(systemIdentity); }
            h.asApp(() -> denied[0].send(h.context(), 0, null, null, null, null, taskOptions.toBundle()));
            check(h.records().size() == 3, "Holder options borrowed a system token's task-placement authority");
        } finally { current.set(null, previousThread); instance.set(singleton, previous); }
    }

    /** Combined lazy-history/URI admission: grants follow the reused record token. */
    private static void reorderedUriOwner() throws Exception {
        Harness h = new Harness();
        String creator = PKG + ".second";
        android.net.Uri uri = android.net.Uri.parse("content://" + creator + ".grants/item");
        Method access = type("UriGrants").getDeclaredMethod("canAccess", ApkPackage.class,
                android.net.Uri.class, int.class, int.class); access.setAccessible(true);
        Method service = h.tasks.getClass().getDeclaredMethod("uriGrants"); service.setAccessible(true);
        Object grants = service.invoke(h.tasks);
        check(!(Boolean)access.invoke(grants, h.owner, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION, 0),
                "Target unexpectedly owns another package's provider");
        check(startFromCreator(h, creator, () -> true,
                launch("Root").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), launch("Middle"),
                launch("Root").setData(uri).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT
                        | Intent.FLAG_GRANT_READ_URI_PERMISSION)) == ActivityManager.START_SUCCESS,
                "URI reorder batch rejected");
        Object root = h.record("Root"), middle = h.record("Middle");
        check(h.records().size() == 2 && h.records().get(1) == root && h.thread.launches(root) == 1
                && h.thread.launches(middle) == 0, "URI reorder broke lazy history or duplicated record");
        check((Boolean)access.invoke(grants, h.owner, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION, 0),
                "Reused activity did not receive its requested URI grant");
        List<?> owned = (List<?>)get(grants, "mGrants");
        check(owned.size() == 1 && get(owned.get(0), "ownerToken") == get(root, "token"),
                "URI grant is not owned by the reordered destination token");
        Intent denied = launch("Middle").setData(uri).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT
                | Intent.FLAG_GRANT_READ_URI_PERMISSION);
        denied.setClipData(android.content.ClipData.newRawUri("denied",
                android.net.Uri.parse("content://1@" + creator + ".grants/denied")));
        try {
            startFromCreator(h, creator, () -> true, denied);
            throw new AssertionError("Invalid URI reordered an admitted task");
        } catch (java.lang.reflect.InvocationTargetException failure) {
            check(failure.getCause() instanceof SecurityException, "Wrong invalid-URI launch outcome");
        }
        check(h.records().size() == 2 && h.records().get(1) == root && owned.size() == 1,
                "Invalid URI changed task order or existing grants");
        h.acknowledge("activityResumed", root); h.finish(root); h.acknowledge("activityDestroyed", root);
        check(!(Boolean)access.invoke(grants, h.owner, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION, 0),
                "Destroyed reordered activity leaked its URI grant");
        check(h.thread.launches(middle) == 1, "Removing URI owner did not realize deferred history");
    }

    private static void abandonedUriOwners() throws Exception {
        for (boolean failedStart : new boolean[]{false, true}) {
            Harness h = new Harness();
            String creator = PKG + ".second", target = PKG + ".third";
            android.net.Uri uri = android.net.Uri.parse("content://" + creator + ".grants/abandoned");
            if (failedStart) {
                Class<?> starterType = type("ActivityTaskManagerService$ProcessStarter");
                Object starter = java.lang.reflect.Proxy.newProxyInstance(starterType.getClassLoader(),
                        new Class<?>[]{starterType}, (proxy, method, args) -> -1);
                Method install = h.tasks.getClass().getDeclaredMethod("setProcessStarter", starterType);
                install.setAccessible(true); install.invoke(h.tasks, starter);
            }
            startFromCreator(h, creator, () -> true, launch(target, "Root").setData(uri)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_GRANT_READ_URI_PERMISSION));
            Method service = h.tasks.getClass().getDeclaredMethod("uriGrants"); service.setAccessible(true);
            Object grants = service.invoke(h.tasks);
            if (!failedStart) {
                check(((List<?>)get(grants, "mGrants")).size() == 1, "Missing pending-history owner grant");
                Method stop = h.tasks.getClass().getDeclaredMethod("forceStopPackage", String.class);
                stop.setAccessible(true); stop.invoke(h.tasks, target);
            }
            check(((List<?>)get(grants, "mGrants")).isEmpty(),
                    "Abandoned activity leaked owner grant after " + (failedStart ? "start failure" : "force stop"));
            check(h.records().isEmpty(), "Abandoned activity history remained after cleanup");
        }
    }

    private static void processDeathUriOwners() throws Exception {
        for (boolean restore : new boolean[]{false, true}) {
            Harness h = new Harness();
            String creator = PKG + ".second";
            android.net.Uri uri = android.net.Uri.parse("content://" + creator + ".grants/death");
            startFromCreator(h, creator, () -> true, launch("Root").setData(uri)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_GRANT_READ_URI_PERMISSION));
            Object root = h.record("Root");
            if (restore) set(root, "savedState", new Bundle());
            Method service = h.tasks.getClass().getDeclaredMethod("uriGrants"); service.setAccessible(true);
            Object grants = service.invoke(h.tasks);
            check(((List<?>)get(grants, "mGrants")).size() == 1, "Missing activity grant before process death");
            Method died = h.tasks.getClass().getDeclaredMethod("processDied", h.process.getClass());
            died.setAccessible(true); died.invoke(h.tasks, h.process);
            check(h.records().contains(root) == restore, "Process death retained/discarded wrong activity record");
            check(((List<?>)get(grants, "mGrants")).isEmpty() != restore,
                    "URI owner lifetime followed process rather than retained activity record");
            if (restore) {
                Method stop = h.tasks.getClass().getDeclaredMethod("forceStopPackage", String.class);
                stop.setAccessible(true); stop.invoke(h.tasks, PKG);
                check(((List<?>)get(grants, "mGrants")).isEmpty(), "Force-stopped restorable record leaked grant");
            }
        }
    }

    private static void forceStopTransientUris() throws Exception {
        Harness h = new Harness();
        Method service = h.tasks.getClass().getDeclaredMethod("uriGrants"); service.setAccessible(true);
        Object grants = service.invoke(h.tasks);
        Method grant = grants.getClass().getDeclaredMethod("grant", String.class, android.net.Uri.class, int.class, int.class);
        grant.setAccessible(true);
        String second = PKG + ".second", third = PKG + ".third";
        grant.invoke(grants, PKG, android.net.Uri.parse("content://" + second + ".grants/incoming"),
                Intent.FLAG_GRANT_READ_URI_PERMISSION, 0);
        grant.invoke(grants, second, android.net.Uri.parse("content://" + PKG + ".grants/outgoing"),
                Intent.FLAG_GRANT_READ_URI_PERMISSION, 0);
        grant.invoke(grants, third, android.net.Uri.parse("content://" + second + ".grants/unrelated"),
                Intent.FLAG_GRANT_READ_URI_PERMISSION, 0);
        List<?> owned = (List<?>)get(grants, "mGrants");
        check(owned.size() == 3, "Transient force-stop fixture did not publish three real grants");
        ((List<?>)get(h.tasks, "mProcesses")).clear(); // Already dead package still needs URI cleanup.
        Method stop = h.tasks.getClass().getDeclaredMethod("forceStopPackage", String.class);
        stop.setAccessible(true); stop.invoke(h.tasks, PKG);
        check(owned.size() == 1 && get(owned.get(0), "ownerToken") == null,
                "Force stop leaked incoming/outgoing ownerless URI grants or removed unrelated grants");
    }

    public static int run(boolean binder, String selected) throws Exception {
        realBinder = binder; checks = 0;
        if (selected.equals("all") || selected.equals("history")) { fullStack(); acceptedHistoryLifetimes(); disabledDeferredHistory(); }
        if (selected.equals("all") || selected.equals("validation")) invalidBatch();
        if (selected.equals("all") || selected.equals("tasks")) { taskFlags(); reorderedUriOwner(); abandonedUriOwners(); processDeathUriOwners(); forceStopTransientUris(); }
        if (selected.equals("all") || selected.equals("attach")) attachAndReentry();
        if (selected.equals("all") || selected.equals("visibility")) visibilityAndRemoval();
        if (selected.equals("all") || selected.equals("permissions")) permissions();
        if (selected.equals("all") || selected.equals("pending")) pendingStacks();
        return checks;
    }
    public static void main(String[] args) throws Exception {
        int total = run(true, args.length == 0 ? "all" : args[0]);
        System.out.println("TaskStackContractTest all passed: " + total
                + " checks; lazy history, Back, task flags, attachment, permissions and public PendingIntent");
    }
}
