/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.IActivityClientController;
import android.app.IActivityManager;
import android.app.IActivityTaskManager;
import android.app.IApplicationThread;
import android.app.PendingIntent;
import android.app.ResultInfo;
import android.app.servertransaction.*;
import android.content.*;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.os.*;
import android.net.Uri;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.android.internal.pm.pkg.component.ParsedIntentInfoImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;
import com.gesher.server.StandIn;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.*;
import sun.misc.Unsafe;

/** Real chooser admission, selected-target launches, transactions, URI grants and callbacks.
 * Only the user-input/native-window boundary is injected; no launch-success dispatcher exists. */
public final class ChooserContractTest {
    private static final String OWNER = "fixture.chooser.owner", FIRST = "fixture.chooser.first", SECOND = "fixture.chooser.second";
    private static final int UID = 12361, PID = 836101;
    private static int checks;
    private static boolean realBinder;
    private static final Unsafe UNSAFE;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe) f.get(null); }
        catch (Exception e) { throw new ExceptionInInitializerError(e); }
    }
    interface Checked { void run() throws Exception; }
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); checks++; }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Object get(Object value, String name) throws Exception { return field(value.getClass(), name).get(value); }
    private static void set(Object value, String name, Object item) throws Exception { field(value.getClass(), name).set(value, item); }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Object make(Class<?> type, Class<?>[] parameters, Object... arguments) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(parameters); c.setAccessible(true); return c.newInstance(arguments);
    }
    private static Object invoke(Object target, String name, Class<?>[] parameters, Object... arguments) throws Exception {
        Method method = target.getClass().getDeclaredMethod(name, parameters); method.setAccessible(true);
        try { return method.invoke(target, arguments); }
        catch (InvocationTargetException e) { if (e.getCause() instanceof Exception) throw (Exception)e.getCause(); throw (Error)e.getCause(); }
    }
    @SuppressWarnings({"unchecked", "rawtypes"}) private static Object state(String name) throws Exception {
        return Enum.valueOf((Class)type("ActivityTaskManagerService$State"), name);
    }
    private static IBinder token(Object record) throws Exception { return (IBinder)get(record, "token"); }
    private static void denied(Class<? extends Throwable> expected, Checked action) throws Exception {
        try { action.run(); } catch (Throwable failure) {
            if (expected.isInstance(failure)) { checks++; return; }
            if (failure instanceof InvocationTargetException && expected.isInstance(failure.getCause())) { checks++; return; }
            throw new AssertionError("Expected " + expected + ", got " + failure, failure);
        }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }
    private static Intent chooser() { return Intent.createChooser(new Intent(Intent.ACTION_SEND).setType("text/plain"), "Fixture share"); }
    private static ApkPackage code(String name, int uid) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36);
        for (String shortName : name.equals(OWNER) ? List.of("Root", "Covered") : List.of("Share", "Initial", "Private", "Guarded")) {
            ParsedActivityImpl a = new ParsedActivityImpl();
            a.setName(name + "." + shortName); a.setPackageName(name); a.setEnabled(true);
            a.setExported(!shortName.equals("Private")); a.setTaskAffinity(name);
            if (shortName.equals("Guarded")) a.setPermission("fixture.chooser.DENIED");
            if (!name.equals(OWNER)) {
                ParsedIntentInfoImpl filter = new ParsedIntentInfoImpl(); filter.setHasDefault(true);
                filter.getIntentFilter().addCategory(Intent.CATEGORY_DEFAULT);
                filter.getIntentFilter().addAction(shortName.equals("Initial") ? "fixture.chooser.INITIAL" : Intent.ACTION_SEND);
                filter.getIntentFilter().addDataType("text/plain"); a.addIntent(filter);
            }
            parsed.addActivity(a);
        }
        ParsedProviderImpl provider = new ParsedProviderImpl();
        provider.setName(name + ".Provider"); provider.setPackageName(name); provider.setEnabled(true);
        provider.setExported(false); provider.setAuthority(name + ".files"); provider.setGrantUriPermissions(true);
        parsed.addProvider(provider); parsed.hideAsFinal();
        ApkPackage code = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(code, "packageName", name); set(code, "parsed", parsed); set(code, "mUid", uid);
        set(code, "apkPath", "/fixture/" + name + "/base.apk"); set(code, "mDataDir", "/fixture/" + name + "/data");
        set(code, "sharedLibraries", SharedLibraryGraph.EMPTY);
        set(code, "mSigningIdentityChecked", true);
        set(code, "mVerifiedSignatures", new android.content.pm.Signature[]{new android.content.pm.Signature(new byte[]{(byte)uid})});
        return code;
    }
    private static final class AppThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        final List<ClientTransactionItem> items = new CopyOnWriteArrayList<>();
        final List<ResultInfo> results = new CopyOnWriteArrayList<>();
        int pid, uid;
        IActivityClientController controller;
        boolean acknowledge = true;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleTransaction(ClientTransaction transaction) {
            long identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)uid << 32) | pid);
                for (ClientTransactionItem item : transaction.getTransactionItems()) {
                    items.add(item);
                    if (item instanceof ActivityResultItem) {
                        @SuppressWarnings("unchecked") List<ResultInfo> values = (List<ResultInfo>)field(ActivityResultItem.class, "mResultInfoList").get(item);
                        results.addAll(values);
                    }
                    if (!acknowledge || !(item instanceof ActivityTransactionItem)) continue;
                    IBinder target = ((ActivityTransactionItem)item).getActivityToken();
                    if (item instanceof PauseActivityItem) controller.activityPaused(target);
                    if (item instanceof ResumeActivityItem) controller.activityResumed(target, false);
                    if (item instanceof StopActivityItem) controller.activityStopped(target, null, null, null, null);
                    if (item instanceof DestroyActivityItem) controller.activityDestroyed(target);
                }
            } catch (Exception e) { throw new AssertionError(e); }
            finally { Binder.restoreCallingIdentity(identity); }
        }
        @Override public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent, int code,
                String data, Bundle extras, boolean ordered, boolean sticky, boolean assumed, int user,
                int processState, int senderUid, String senderPackage) throws RemoteException {
            receiver.performReceive(intent, code, data, extras, ordered, sticky, user);
        }
        int count(Class<?> kind) { int count = 0; for (Object item : items) if (kind.isInstance(item)) count++; return count; }
    }
    private static final class Selection {
        final CountDownLatch shown = new CountDownLatch(1), release = new CountDownLatch(1);
        volatile Thread worker;
        volatile String[] labels;
        volatile BooleanSupplier active;
        volatile int selected = -1;
        volatile boolean available = true;
        volatile Throwable failure;
        Object dialog() throws Exception {
            Class<?> api = type("IntentChooser$Dialog");
            return Proxy.newProxyInstance(api.getClassLoader(), new Class<?>[]{api}, (p, m, args) -> {
                if (m.getName().equals("available")) return available;
                worker = Thread.currentThread();
                labels = (String[])args[2]; active = (BooleanSupplier)args[4]; shown.countDown();
                if (!release.await(5, TimeUnit.SECONDS)) throw new AssertionError("Fixture input was not released");
                if (failure != null) throw failure;
                return selected;
            });
        }
        void displayed() throws Exception { check(shown.await(3, TimeUnit.SECONDS), "Production chooser never requested user input"); }
        void choose(int index) { selected = index; release.countDown(); }
    }
    private static final class Harness {
        final PackageManagerService pm;
        final Object tasks, ownerProcess, root;
        final IActivityTaskManager api;
        final IActivityManager am;
        final Class<?> recordType = type("ActivityTaskManagerService$ActivityRecord");
        final AppThread owner = new AppThread(), first = new AppThread(), second = new AppThread();
        final Selection ui = new Selection();
        final ApkPackage sender = code(OWNER, UID), target1, target2 = code(SECOND, UID + 2);
        Harness() throws Exception { this(UID + 1); }
        @SuppressWarnings("unchecked") Harness(int firstUid) throws Exception {
            target1 = code(FIRST, firstUid);
            pm = new PackageManagerService(List.of(sender, target1, target2));
            set(pm, "mPackageEventSender", (java.util.function.Consumer<Intent>)event -> { });
            tasks = make(type("ActivityTaskManagerService"), new Class<?>[]{PackageManagerService.class}, pm);
            am = (IActivityManager)make(type("ActivityManagerService"), new Class<?>[]{tasks.getClass(), PackageManagerService.class,
                    type("NotificationManagerService")}, tasks, pm, null);
            Object permissions = invoke(pm, "permissions", new Class<?>[0]);
            Object catalog = get(permissions, "mCatalog"); set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            ownerProcess = process(OWNER, UID, PID, owner); process(FIRST, firstUid, PID + 1, first); process(SECOND, UID + 2, PID + 2, second);
            root = record("Root", ownerProcess, "RESUMED");
            set(tasks, "mNextTaskId", 10);
            set(tasks, "mIntentChooserDialog", ui.dialog());
            if (!realBinder) api = (IActivityTaskManager)tasks;
            else {
                IBinder target = StandIn.of(IActivityTaskManager.class, (IActivityTaskManager)tasks).asBinder();
                api = IActivityTaskManager.Stub.asInterface(new Binder() {
                    @Override public IInterface queryLocalInterface(String descriptor) { return null; }
                    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                        return target.transact(code, data, reply, flags);
                    }
                });
            }
        }
        @SuppressWarnings("unchecked") Object process(String name, int uid, int pid, AppThread thread) throws Exception {
            thread.pid = pid; thread.uid = uid;
            Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
            Object process = make(processType, new Class<?>[]{tasks.getClass(), int.class, IApplicationThread.class}, tasks, pid, thread);
            set(process, "uid", uid); set(process, "packageName", name); set(process, "processName", name); set(process, "ready", true);
            set(process, "packageCode", pm.get(name));
            ((List<Object>)get(tasks, "mProcesses")).add(process);
            thread.controller = (IActivityClientController)get(tasks, "mClientImpl");
            return process;
        }
        @SuppressWarnings("unchecked") Object record(String name, Object process, String state) throws Exception {
            ActivityInfo info = pm.getActivityInfo(new ComponentName(OWNER, OWNER + "." + name), 0, 0);
            Object record = make(recordType, new Class<?>[]{tasks.getClass(), ActivityInfo.class, Intent.class,
                    int.class, int.class, String.class, recordType, String.class, int.class},
                    tasks, info, new Intent().setComponent(new ComponentName(info.packageName, info.name)), 1, PID, OWNER, null, null, -1);
            set(record, "app", process); set(record, "state", state(state));
            ((List<Object>)get(tasks, "mActivities")).add(record); ((List<Object>)get(process, "activities")).add(record);
            List<Integer> order = (List<Integer>)get(tasks, "mTaskOrder"); if (!order.contains(1)) order.add(1);
            return record;
        }
        void asOwner(Checked action) throws Exception {
            long previous = Binder.clearCallingIdentity();
            try { Binder.restoreCallingIdentity(((long)UID << 32) | PID); action.run(); }
            finally { Binder.restoreCallingIdentity(previous); }
        }
        int start(Intent chooser) throws Exception { return start(chooser, token(root), 51); }
        int start(Intent chooser, IBinder source, int request) throws Exception {
            AtomicInteger result = new AtomicInteger();
            asOwner(() -> result.set(api.startActivity(owner, OWNER, null, chooser, null, source, "share", request, 0, null, null)));
            return result.get();
        }
        @SuppressWarnings("unchecked") List<Object> records() throws Exception { return (List<Object>)get(tasks, "mActivities"); }
        Object launched(String name) throws Exception {
            synchronized (tasks) {
                for (Object record : records()) if (((ActivityInfo)get(record, "info")).name.equals(name)) return record;
            }
            return null;
        }
        int pending() throws Exception { synchronized (tasks) { return ((Map<?,?>)get(tasks, "mIntentChoosers")).size(); } }
        long idleSince() throws Exception { return (Long)invoke(tasks, "idleSince", new Class<?>[0]); }
        @SuppressWarnings("unchecked") void withoutActivities() throws Exception {
            synchronized (tasks) {
                records().clear();
                for (Object process : (List<?>)get(tasks, "mProcesses")) ((List<?>)get(process, "activities")).clear();
                ((List<Integer>)get(tasks, "mTaskOrder")).clear();
                // An existing idle interval must not survive a complete UI operation,
                // even when the idle monitor never polls while that UI is open.
                set(tasks, "mIdleSince", 1L);
            }
        }
        void settled() throws Exception {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            while (pending() != 0 && System.nanoTime() < deadline) Thread.sleep(5);
            check(pending() == 0, "Chooser reservation was not released");
            // The reservation is retired before resume/callback scheduling; wait for that worker.
            for (Thread t : Thread.getAllStackTraces().keySet()) if (t.getName().equals("intent-chooser")) {
                t.join(1000); check(!t.isAlive(), "Chooser worker retained after completion");
            }
        }
        boolean access(ApkPackage target, Uri uri, int mode) throws Exception {
            Object grants = invoke(tasks, "uriGrants", new Class<?>[0]);
            return (Boolean)invoke(grants, "canAccess", new Class<?>[]{ApkPackage.class, Uri.class, int.class, int.class}, target, uri, mode, 0);
        }
    }
    private static void validation() throws Exception {
        Harness h = new Harness();
        denied(IllegalArgumentException.class, () -> h.start(new Intent(Intent.ACTION_CHOOSER)));
        denied(IllegalArgumentException.class, () -> h.start(new Intent(Intent.ACTION_CHOOSER).putExtra(Intent.EXTRA_INTENT, "wrong")));
        denied(IllegalArgumentException.class, () -> h.start(Intent.createChooser(new Intent(Intent.ACTION_CHOOSER), "recursive")));
        denied(IllegalArgumentException.class, () -> h.start(chooser().putExtra(Intent.EXTRA_INITIAL_INTENTS, "wrong")));
        denied(IllegalArgumentException.class, () -> h.start(chooser().putExtra(Intent.EXTRA_INITIAL_INTENTS, new Parcelable[]{new Bundle()})));
        denied(IllegalArgumentException.class, () -> h.start(chooser().putExtra(Intent.EXTRA_EXCLUDE_COMPONENTS, new Parcelable[]{new Intent()})));
        denied(IllegalArgumentException.class, () -> h.start(chooser().putExtra(Intent.EXTRA_CHOSEN_COMPONENT_INTENT_SENDER, "wrong")));
        check(h.start(chooser().putExtra(Intent.EXTRA_CHOOSER_REFINEMENT_INTENT_SENDER, new Intent()))
                == ActivityManager.START_INTENT_NOT_RESOLVED, "Unsupported refinement was silently bypassed");
        denied(SecurityException.class, () -> h.start(chooser(), new Binder(), 51));
        android.app.ActivityOptions taskOptions = android.app.ActivityOptions.makeBasic();
        taskOptions.setLaunchTaskId(1);
        h.asOwner(() -> denied(SecurityException.class, () -> h.api.startActivity(h.owner, OWNER, null,
                chooser(), null, token(h.root), "share", 51, 0, null, taskOptions.toBundle())));
        h.asOwner(() -> denied(SecurityException.class, () -> h.api.startActivityAsUser(h.owner, OWNER, null,
                chooser(), null, token(h.root), "share", 51, 0, null, null, 1)));
        Intent empty = chooser().putExtra(Intent.EXTRA_EXCLUDE_COMPONENTS, new ComponentName[]{
                new ComponentName(FIRST, FIRST + ".Share"), new ComponentName(SECOND, SECOND + ".Share")});
        check(h.start(empty) == ActivityManager.START_INTENT_NOT_RESOLVED, "Exclusions or permission/export filtering failed");
        check(h.pending() == 0 && h.records().size() == 1, "Rejected chooser changed lifecycle");
        h.ui.available = false;
        check(h.start(chooser()) == ActivityManager.START_INTENT_NOT_RESOLVED, "Unavailable host backend fabricated success");
        set(h.tasks, "mIntentChooserDialog", null);
        check(h.start(chooser()) == ActivityManager.START_INTENT_NOT_RESOLVED, "Null backend auto-selected a target");
        check(h.owner.count(PauseActivityItem.class) == 0, "Unavailable chooser paused caller");
        Harness explicit = new Harness();
        check(explicit.start(new Intent(Intent.ACTION_CHOOSER).setComponent(new ComponentName(SECOND, SECOND + ".Share")))
                == ActivityManager.START_SUCCESS && explicit.launched(SECOND + ".Share") != null
                && explicit.ui.shown.getCount() == 1, "Explicit app ACTION_CHOOSER routing was intercepted");
    }
    private static void selectionAndGrants() throws Exception {
        Harness h = new Harness();
        Uri uri = Uri.parse("content://" + OWNER + ".files/shared");
        Intent target = new Intent(Intent.ACTION_SEND).setType("text/plain").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION).putExtra(Intent.EXTRA_STREAM, uri);
        Intent request = Intent.createChooser(target, "Choose second");
        check(h.start(request) == ActivityManager.START_SUCCESS, "Interactive chooser was not admitted");
        h.ui.displayed();
        check(h.ui.labels.length == 2 && h.launched(FIRST + ".Share") == null && h.launched(SECOND + ".Share") == null,
                "A target launched before user choice, or private/permission-guarded rows leaked");
        check(h.owner.count(PauseActivityItem.class) == 1 && get(h.root, "state").toString().equals("PAUSED"), "Caller did not pause before host input");
        check(!h.access(h.target1, uri, 1) && !h.access(h.target2, uri, 1), "Chooser presentation leaked URI grants");
        target.setType("image/png"); target.putExtra(Intent.EXTRA_STREAM, Uri.parse("content://changed.example/wrong"));
        h.ui.choose(1); h.settled();
        Object launched = h.launched(SECOND + ".Share");
        check(launched != null && h.launched(FIRST + ".Share") == null, "Selected component was not the only admitted target");
        check(h.second.count(LaunchActivityItem.class) == 1 && h.second.count(ResumeActivityItem.class) == 1,
                "Selected target did not receive the normal launch/resume transaction");
        check(get(launched, "resultTo") == h.root && ((Integer)get(launched, "requestCode")) == 51,
                "Chooser lost the original activity result route");
        Intent received = (Intent)get(launched, "intent");
        check("text/plain".equals(received.getType()) && uri.equals(received.getParcelableExtra(Intent.EXTRA_STREAM, Uri.class)),
                "Queued share changed after the caller mutated its original intent");
        check(h.access(h.target2, uri, 1) && !h.access(h.target2, uri, 2) && !h.access(h.target1, uri, 1),
                "Selected-target URI grant was absent, broadened, or given to an unselected app");
        long identity = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long)(UID + 2) << 32) | (PID + 2));
            h.second.controller.finishActivity(token(launched), 17, new Intent("fixture.RESULT"), 0);
        } finally { Binder.restoreCallingIdentity(identity); }
        check(!h.access(h.target2, uri, 1), "Finished target retained its activity-owned URI grant");
        check(h.owner.results.size() == 1 && h.owner.results.get(0).mResultCode == 17,
                "Chosen activity's result did not reach the original caller");
    }
    private static void parcelledSharePreparation() throws Exception {
        if (!realBinder) return; // This regression needs the real Android Parcel reader.
        Harness h = new Harness();
        Method prepare = type("IntentChooser").getDeclaredMethod("prepare", Intent.class,
                Intent.class, android.content.pm.ApplicationInfo.class);
        prepare.setAccessible(true);
        Uri first = Uri.parse("content://" + OWNER + ".files/first");
        Uri second = Uri.parse("content://" + OWNER + ".files/second");
        for (boolean multiple : new boolean[]{false, true}) {
            Intent original = new Intent(multiple ? Intent.ACTION_SEND_MULTIPLE : Intent.ACTION_SEND)
                    .setType("text/plain").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            if (multiple) original.putParcelableArrayListExtra(Intent.EXTRA_STREAM,
                    new ArrayList<>(List.of(first, second)));
            else original.putExtra(Intent.EXTRA_STREAM, first).putExtra(Intent.EXTRA_TEXT, "shared text");
            Parcel parcel = Parcel.obtain();
            Intent transported;
            try {
                original.writeToParcel(parcel, 0); parcel.setDataPosition(0);
                transported = Intent.CREATOR.createFromParcel(parcel);
            } finally { parcel.recycle(); }
            check(transported.getExtras().isParcelled(), "Fixture did not retain a parcelled share payload");
            Intent prepared = (Intent)prepare.invoke(null, transported,
                    Intent.createChooser(new Intent(), "Transported"), h.sender.applicationInfo(0));
            ClipData clip = prepared.getClipData();
            check(clip != null && clip.getItemCount() == (multiple ? 2 : 1)
                    && first.equals(clip.getItemAt(0).getUri())
                    && (!multiple || second.equals(clip.getItemAt(1).getUri()))
                    && (multiple || "shared text".contentEquals(clip.getItemAt(0).getText()))
                    && (prepared.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0
                    && (prepared.getFlags() & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) == 0,
                    "Parcelled share lost its exact payload or broadened URI permission");
        }
    }
    private static void cancellation() throws Exception {
        Harness h = new Harness(); h.start(chooser()); h.ui.displayed();
        check(h.start(chooser()) == ActivityManager.START_CANCELED, "Second pending chooser replaced the first");
        h.ui.choose(-1); h.settled();
        check(h.records().size() == 1 && h.owner.results.size() == 1
                && h.owner.results.get(0).mResultCode == Activity.RESULT_CANCELED, "Cancel launched a target or lost/doubled the result");
        check(h.owner.count(ResumeActivityItem.class) == 1 && ((Integer)get(h.root, "chooserCoverCount")) == 0,
                "Cancel did not restore caller lifecycle and release coverage");
        Harness late = new Harness(); late.owner.acknowledge = false; late.start(chooser()); late.ui.displayed();
        late.ui.choose(99); late.settled();
        check(late.owner.count(ResumeActivityItem.class) == 0, "Cancel resumed before onPause acknowledgement");
        late.owner.acknowledge = true;
        late.asOwner(() -> late.owner.controller.activityPaused(token(late.root)));
        check(late.owner.count(ResumeActivityItem.class) == 1 && late.owner.results.size() == 1,
                "Late pause acknowledgement stranded caller or cancellation result");
        Harness top = new Harness(); set(top.root, "state", state("STOPPED"));
        Object covered = top.record("Covered", top.ownerProcess, "RESUMED");
        top.start(chooser()); top.ui.displayed(); top.ui.choose(-1); top.settled();
        check(get(top.root, "state").toString().equals("STOPPED") && get(covered, "state").toString().equals("RESUMED"),
                "Cancel resumed result recipient instead of covered task top");
        check(((List<?>)get(top.root, "pendingResults")).size() == 1, "Stopped recipient lost chooser cancellation");
    }
    private static void initialsAndSingle() throws Exception {
        Harness h = new Harness();
        Intent initial = new Intent("fixture.chooser.INITIAL").setType("text/plain").setPackage(FIRST);
        Intent request = chooser().putExtra(Intent.EXTRA_INITIAL_INTENTS,
                new Intent[]{new Intent("fixture.NO_MATCH"), initial, new Intent().setComponent(new ComponentName(SECOND, SECOND + ".Initial"))});
        h.start(request); h.ui.displayed();
        check(h.ui.labels.length == 3, "Unresolved or third initial intent was offered");
        h.ui.choose(0); h.settled();
        check(h.launched(FIRST + ".Initial") != null && h.launched(SECOND + ".Initial") == null,
                "Initial choice was not resolved to its admitted component");
        Harness capped = new Harness();
        capped.start(chooser().putExtra(Intent.EXTRA_INITIAL_INTENTS,
                new Parcelable[]{new Intent("fixture.NO_MATCH"), initial, new Bundle()}));
        capped.ui.displayed();
        check(capped.ui.labels.length == 3, "Ignored third initial entry changed the capped request");
        capped.ui.choose(-1); capped.settled();
        Harness single = new Harness();
        Intent one = chooser().addFlags(Intent.FLAG_ACTIVITY_NO_USER_ACTION)
                .putExtra(Intent.EXTRA_EXCLUDE_COMPONENTS, new ComponentName[]{new ComponentName(FIRST, FIRST + ".Share")});
        check(single.start(one) == ActivityManager.START_SUCCESS && single.launched(SECOND + ".Share") != null,
                "Eligible sole target was not auto-launched");
        check(single.ui.shown.getCount() == 1, "Auto-launch incorrectly requested input");
        PauseActivityItem autoPause = null;
        for (ClientTransactionItem item : single.owner.items) if (item instanceof PauseActivityItem) autoPause = (PauseActivityItem)item;
        check(autoPause != null && !(Boolean)get(autoPause, "mUserLeaving"), "Auto-launch lost the chooser's NO_USER_ACTION pause");
        Harness forced = new Harness(); one.putExtra(Intent.EXTRA_AUTO_LAUNCH_SINGLE_CHOICE, false);
        forced.start(one); forced.ui.displayed();
        check(forced.launched(SECOND + ".Share") == null && forced.ui.labels.length == 1, "Explicit false auto-launch preference was ignored");
        forced.ui.choose(-1); forced.settled();
    }
    private static void sharedUid() throws Exception {
        Harness h = new Harness(UID);
        h.start(chooser()); h.ui.displayed();
        int privateIndex = -1;
        boolean guarded = false;
        for (int i = 0; i < h.ui.labels.length; ++i) {
            if (h.ui.labels[i].contains(FIRST + ".Private")) privateIndex = i;
            if (h.ui.labels[i].contains(FIRST + ".Guarded")) guarded = true;
            check(!h.ui.labels[i].contains(SECOND + ".Private") && !h.ui.labels[i].contains(SECOND + ".Guarded"),
                    "Unrelated UID acquired private or permission-protected choices");
        }
        check(privateIndex >= 0 && guarded, "Shared UID did not inherit component-owner access");
        h.ui.choose(privateIndex); h.settled();
        check(h.launched(FIRST + ".Private") != null, "Normal activity start rejected the chooser's same-UID private target");
        Harness direct = new Harness(UID);
        check(direct.start(new Intent().setComponent(new ComponentName(FIRST, FIRST + ".Guarded"))) == ActivityManager.START_SUCCESS,
                "Normal activity start rejected same-UID component permission access");
        for (String name : List.of("Private", "Guarded")) {
            Harness unrelated = new Harness();
            denied(SecurityException.class, () -> unrelated.start(new Intent().setComponent(new ComponentName(FIRST, FIRST + "." + name))));
        }
    }

    private static Intent envelope(Intent selector) {
        Intent outer = new Intent("fixture.chooser.ENVELOPE").setType("image/png").putExtra("payload", "outer");
        outer.setSelector(selector);
        return outer;
    }

    private static void selectors() throws Exception {
        for (String kind : List.of("primary", "initial", "alternate")) {
            Harness h = new Harness();
            Intent selection = new Intent(kind.equals("initial") ? "fixture.chooser.INITIAL" : Intent.ACTION_SEND).setType("text/plain");
            if (kind.equals("initial")) selection.setPackage(FIRST);
            Intent outer = envelope(selection);
            Intent request = kind.equals("primary") ? Intent.createChooser(outer, "Selector") : chooser();
            if (kind.equals("initial")) request.putExtra(Intent.EXTRA_INITIAL_INTENTS, new Intent[]{outer});
            if (kind.equals("alternate")) request = Intent.createChooser(new Intent("fixture.NO_MATCH"), "Alternate")
                    .putExtra(Intent.EXTRA_ALTERNATE_INTENTS, new Intent[]{outer});
            h.start(request); h.ui.displayed();
            check(h.ui.labels.length == (kind.equals("initial") ? 3 : 2), "Selector was ignored for " + kind);
            h.ui.choose(0); h.settled();
            Object target = h.launched(FIRST + (kind.equals("initial") ? ".Initial" : ".Share"));
            check(target != null, "Selector choice was not published for " + kind);
            Intent delivered = (Intent)get(target, "intent");
            check("fixture.chooser.ENVELOPE".equals(delivered.getAction()) && "image/png".equals(delivered.getType())
                    && "outer".equals(delivered.getStringExtra("payload")) && delivered.getSelector() != null
                    && "text/plain".equals(delivered.getSelector().getType()), "Selector replaced the delivered outer payload for " + kind);
        }
        Harness explicit = new Harness();
        Intent outer = envelope(new Intent().setComponent(new ComponentName(SECOND, SECOND + ".Share")))
                .setComponent(new ComponentName(FIRST, FIRST + ".Share"));
        check(explicit.start(Intent.createChooser(outer, "Explicit")) == ActivityManager.START_SUCCESS
                && explicit.launched(FIRST + ".Share") != null, "Selector overrode an explicit outer component");
    }

    /** The provider endpoint is the isolated fixture; MIME lookup, registry admission,
     * ContentResolver protocol, and activity/grant publication remain production code. */
    private static final class MimeFixture implements AutoCloseable {
        final Harness h;
        final Object providers;
        final Object serviceSingleton = field(ActivityManager.class, "IActivityManagerSingleton").get(null);
        final Field serviceField = field(android.util.Singleton.class, "mInstance");
        final Object previous;
        final AtomicInteger queries = new AtomicInteger(), anonymousQueries = new AtomicInteger();
        final AtomicInteger fullTypeCalls = new AtomicInteger(), anonymousTypeCalls = new AtomicInteger();
        volatile String answer = "text/plain", anonymousAnswer;
        volatile boolean failure, heldConnection;
        MimeFixture(Harness h) throws Exception { this(h, OWNER); }
        MimeFixture(Harness h, String packageName) throws Exception {
            this.h = h;
            providers = get(h.am, "mProviders");
            ApkPackage code = h.pm.get(packageName);
            Object process = null;
            for (Object candidate : (List<?>)get(h.tasks, "mProcesses")) {
                if (packageName.equals(get(candidate, "packageName"))) { process = candidate; break; }
            }
            final Object publisher = process;
            invoke(providers, "providersFor", new Class<?>[]{publisher.getClass(), android.content.pm.ApplicationInfo.class},
                    publisher, code.applicationInfo(0));
            android.app.ContentProviderHolder holder = new android.app.ContentProviderHolder(
                    h.pm.getProviderInfo(new ComponentName(packageName, packageName + ".Provider"), 0, 0));
            ContentProvider endpoint = new ContentProvider() {
                @Override public boolean onCreate() { return true; }
                @Override public String getType(Uri uri) {
                    fullTypeCalls.incrementAndGet();
                    if (failure) throw new IllegalArgumentException("Fixture has no MIME result");
                    return answer;
                }
                @Override public String getTypeAnonymous(Uri uri) {
                    anonymousTypeCalls.incrementAndGet();
                    return anonymousAnswer;
                }
                @Override public android.database.Cursor query(Uri uri, String[] projection, String selection,
                        String[] arguments, String order) { throw new AssertionError("MIME resolution queried payload data"); }
                @Override public Uri insert(Uri uri, ContentValues values) { throw new AssertionError("MIME resolution inserted data"); }
                @Override public int update(Uri uri, ContentValues values, String selection, String[] arguments) {
                    throw new AssertionError("MIME resolution updated data");
                }
                @Override public int delete(Uri uri, String selection, String[] arguments) {
                    throw new AssertionError("MIME resolution deleted data");
                }
            };
            endpoint.attachInfo(new ContextWrapper(null) {
                @Override public String getPackageName() { return packageName; }
                @Override public String getOpPackageName() { return packageName; }
                @Override public int getUserId() { return 0; }
                @Override public android.content.pm.ApplicationInfo getApplicationInfo() { return code.applicationInfo(0); }
                @Override public AttributionSource getAttributionSource() { return new AttributionSource(code.uid(), packageName, null); }
                @Override public Object getSystemService(String name) { return null; }
            }, holder.info);
            IContentProvider transport = endpoint.getIContentProvider();
            holder.provider = (IContentProvider)Proxy.newProxyInstance(IContentProvider.class.getClassLoader(),
                    new Class<?>[]{IContentProvider.class}, (proxy, method, args) -> {
                if (method.getName().equals("asBinder")) return transport.asBinder();
                boolean anonymous = method.getName().equals("getTypeAnonymousAsync");
                if (!anonymous && !method.getName().equals("getTypeAsync")) throw new AssertionError("Unexpected provider call: " + method.getName());
                if (anonymous) anonymousQueries.incrementAndGet();
                else {
                    queries.incrementAndGet();
                    heldConnection |= !((Map<?,?>)get(providers, "mConnections")).isEmpty();
                    AttributionSource source = (AttributionSource)args[0];
                    check(source.getUid() == android.os.Process.SYSTEM_UID && "android".equals(source.getPackageName())
                            && h.pm.checkUidPermission(android.Manifest.permission.GET_ANY_PROVIDER_TYPE, source.getUid())
                                    == PackageManager.PERMISSION_GRANTED,
                            "Chooser MIME lookup lacks its privileged metadata identity");
                }
                check(!Thread.holdsLock(h.pm) && !Thread.holdsLock(get(h.tasks, "mLaunchLock")),
                        "Provider callback ran under a package or activity-launch monitor");
                // Observe the boundary, then execute the actual framework transport's
                // MIME permission checks, callback protocol and exception conversion.
                try { return method.invoke(transport, args); }
                catch (InvocationTargetException e) { throw e.getCause(); }
            });
            long identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)code.uid() << 32) | (Integer)get(publisher, "pid"));
                h.am.publishContentProviders((IApplicationThread)get(publisher, "thread"), List.of(holder));
            } finally { Binder.restoreCallingIdentity(identity); }
            previous = serviceField.get(serviceSingleton); serviceField.set(serviceSingleton, h.am);
        }
        ContentResolver resolver() throws Exception {
            return (ContentResolver)invoke(h.am, "mimeResolver",
                    new Class<?>[]{android.content.pm.ApplicationInfo.class, h.ownerProcess.getClass()},
                    h.sender.applicationInfo(0), h.ownerProcess);
        }
        boolean released() throws Exception { return ((Map<?,?>)get(providers, "mConnections")).isEmpty(); }
        @Override public void close() throws Exception { serviceField.set(serviceSingleton, previous); }
    }

    private static void providerTypes() throws Exception {
        for (boolean selector : new boolean[]{false, true}) {
            Harness h = new Harness();
            try (MimeFixture provider = new MimeFixture(h)) {
                Uri uri = Uri.parse("content://" + OWNER + ".files/mime");
                long identity = Binder.clearCallingIdentity();
                try {
                    check("text/plain".equals(provider.resolver().getType(uri)),
                            "Published provider did not supply MIME through the real ContentResolver");
                } finally { Binder.restoreCallingIdentity(identity); }
                provider.queries.set(0);
                Intent data = new Intent(Intent.ACTION_SEND).setData(uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                Intent payload = selector ? envelope(data) : data;
                check(h.start(Intent.createChooser(payload, "Provider MIME")) == ActivityManager.START_SUCCESS,
                        "Content-provider MIME inference did not resolve its real handlers");
                h.ui.displayed();
                check(h.ui.labels.length == 2 && provider.queries.get() == 1 && provider.released(),
                        "MIME inference skipped its provider or retained its holder");
                h.ui.choose(1); h.settled();
                Object target = h.launched(SECOND + ".Share");
                check(target != null, "Provider-resolved target did not launch");
                Intent delivered = (Intent)get(target, "intent");
                check(selector ? "image/png".equals(delivered.getType()) && delivered.getSelector().getType() == null
                        : delivered.getType() == null && uri.equals(delivered.getData()), "Inferred MIME was written over the original payload");
                if (!selector) check(h.access(h.target2, uri, 1) && !h.access(h.target1, uri, 1),
                        "MIME lookup replaced selected-target URI-grant semantics");
            }
        }
        for (String kind : List.of("unknown", "failure", "explicit")) {
            Harness h = new Harness();
            try (MimeFixture provider = new MimeFixture(h, SECOND)) {
                provider.answer = null; provider.failure = kind.equals("failure");
                Intent payload = new Intent(Intent.ACTION_SEND).setData(Uri.parse("content://" + SECOND + ".files/no-type"));
                if (kind.equals("explicit")) payload.setDataAndType(payload.getData(), "text/plain");
                int result = h.start(Intent.createChooser(payload, "MIME availability"));
                if (kind.equals("explicit")) {
                    h.ui.displayed(); check(provider.queries.get() == 0, "Explicit MIME unnecessarily queried the provider");
                    h.ui.choose(-1); h.settled();
                } else check(result == ActivityManager.START_INTENT_NOT_RESOLVED && provider.queries.get() == 1 && provider.heldConnection
                        && h.owner.count(PauseActivityItem.class) == 0, "Unknown or failed MIME invented a usable type");
                check(provider.released(), "Failed MIME lookup retained its provider holder");
            }
        }
        for (boolean publicType : new boolean[]{false, true}) {
            Harness h = new Harness();
            try (MimeFixture provider = new MimeFixture(h, SECOND)) {
                provider.anonymousAnswer = publicType ? "text/plain" : "application/octet-stream";
                Uri uri = Uri.parse("content://" + SECOND + ".files/private");
                check(h.pm.checkUidPermission(android.Manifest.permission.GET_ANY_PROVIDER_TYPE, UID)
                        == PackageManager.PERMISSION_DENIED, "Fixture creator unexpectedly has MIME metadata privilege");
                h.asOwner(() -> denied(SecurityException.class,
                        () -> h.am.getContentProvider(h.owner, OWNER, SECOND + ".files", 0, true)));
                Intent payload = new Intent(Intent.ACTION_SEND).setData(uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                check(h.start(Intent.createChooser(payload, "Protected MIME metadata")) == ActivityManager.START_SUCCESS,
                        "Chooser failed to resolve protected full MIME metadata");
                h.ui.displayed();
                check(h.ui.labels.length == 2 && provider.queries.get() == 1 && provider.anonymousQueries.get() == 0
                        && provider.fullTypeCalls.get() == 1 && provider.anonymousTypeCalls.get() == 0
                        && provider.heldConnection && provider.released(),
                        "Privileged chooser MIME used anonymous data or retained its provider connection");
                check(!h.access(h.sender, uri, 1) && !h.access(h.target1, uri, 1),
                        "Discovering protected MIME conferred URI access");
                h.ui.choose(0); h.settled();
                check(h.launched(FIRST + ".Share") == null && !h.access(h.sender, uri, 1) && !h.access(h.target1, uri, 1),
                        "Privileged MIME metadata authorized the creator's forbidden URI transfer");
                check(h.owner.results.size() == 1 && h.owner.results.get(0).mResultCode == Activity.RESULT_CANCELED
                        && h.owner.count(ResumeActivityItem.class) == 1,
                        "Denied protected-URI selection lost cancellation or caller resumption");
                h.asOwner(() -> denied(SecurityException.class,
                        () -> h.am.getContentProvider(h.owner, OWNER, SECOND + ".files", 0, true)));
            }
        }
        Harness granted = new Harness();
        try (MimeFixture provider = new MimeFixture(granted, SECOND)) {
            Uri uri = Uri.parse("content://" + SECOND + ".files/granted");
            long identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)(UID + 2) << 32) | (PID + 2));
                granted.am.grantUriPermission(granted.second, OWNER, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION, 0);
            } finally { Binder.restoreCallingIdentity(identity); }
            Intent payload = new Intent(Intent.ACTION_SEND).setData(uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            granted.start(Intent.createChooser(payload, "Granted provider MIME")); granted.ui.displayed();
            check(provider.queries.get() == 1 && provider.anonymousQueries.get() == 0
                    && provider.heldConnection && provider.released(), "Granted MIME lookup failed to hold and release its real provider connection");
            granted.ui.choose(0); granted.settled();
            check(granted.launched(FIRST + ".Share") != null && granted.access(granted.target1, uri, 1)
                    && !granted.access(granted.target1, uri, 2), "Granted MIME sharing lost its bounded original URI authority");
        }
    }

    private static void retirement() throws Exception {
        for (String reason : List.of("process", "activity", "caller-code", "target-code", "permission")) {
            Harness h = new Harness(); h.start(chooser()); h.ui.displayed();
            if (reason.equals("process")) invoke(h.tasks, "processDied", new Class<?>[]{h.ownerProcess.getClass()}, h.ownerProcess);
            if (reason.equals("activity")) h.asOwner(() -> h.owner.controller.finishActivity(token(h.root), 0, null, 0));
            if (reason.equals("caller-code") || reason.equals("target-code")) {
                @SuppressWarnings("unchecked") Map<String, ApkPackage> packages = (Map<String, ApkPackage>)get(h.pm, "mPackages");
                String retired = reason.equals("caller-code") ? OWNER : FIRST;
                packages.put(retired, code(retired, reason.equals("caller-code") ? UID : UID + 1));
            }
            if (reason.equals("permission")) {
                // The selected component can become disabled while its native dialog is open.
                h.pm.setComponentEnabledSetting(new ComponentName(FIRST, FIRST + ".Share"),
                        PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP, 0, OWNER);
            }
            if (reason.equals("activity") || reason.equals("process")) {
                check(!h.ui.active.getAsBoolean() && h.idleSince() == 0,
                        "Retirement made the device idle before the native chooser closed");
            }
            h.ui.choose(0); h.settled();
            check(h.launched(FIRST + ".Share") == null, "Retired choice launched after " + reason);
            check(((Integer)get(h.root, "chooserCoverCount")) == 0, "Retirement leaked lifecycle coverage: " + reason);
            if (reason.equals("permission")) h.pm.setComponentEnabledSetting(new ComponentName(FIRST, FIRST + ".Share"),
                    PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, PackageManager.DONT_KILL_APP, 0, OWNER);
        }
        Harness h = new Harness();
        Intent deniedGrant = chooser(); Intent target = deniedGrant.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
        target.setClipData(ClipData.newRawUri("private", Uri.parse("content://" + SECOND + ".files/private")));
        target.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        h.start(deniedGrant); h.ui.displayed(); h.ui.choose(0); h.settled();
        check(h.launched(FIRST + ".Share") == null && h.owner.results.size() == 1,
                "System chooser laundered unauthorized URI authority or failed to cancel");
    }
    private static void flagsAndSuspension() throws Exception {
        Harness h = new Harness();
        Intent request = chooser().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_NO_USER_ACTION);
        h.start(request); h.ui.displayed();
        check(h.owner.results.size() == 1 && h.owner.results.get(0).mResultCode == Activity.RESULT_CANCELED,
                "NEW_TASK did not cancel the activity-result dependency at admission");
        PauseActivityItem pause = null;
        for (ClientTransactionItem item : h.owner.items) if (item instanceof PauseActivityItem) pause = (PauseActivityItem)item;
        check(pause != null && !(Boolean)get(pause, "mUserLeaving"), "NO_USER_ACTION was lost by native chooser pause");
        h.ui.choose(0); h.settled();
        Object target = h.launched(FIRST + ".Share");
        check(target != null && ((Integer)get(target, "taskId")) != 1 && get(target, "resultTo") == null
                && h.owner.results.size() == 1, "NEW_TASK selection inherited an old result route or duplicate cancellation");
        Harness hidden = new Harness(); hidden.start(chooser()); hidden.ui.displayed();
        hidden.asOwner(() -> check(hidden.owner.controller.moveActivityTaskToBack(token(hidden.root), true), "Could not hide chooser task"));
        check(!hidden.ui.active.getAsBoolean(), "Hidden task retained an active chooser");
        hidden.ui.choose(0); hidden.settled();
        check(hidden.launched(FIRST + ".Share") == null && hidden.owner.count(ResumeActivityItem.class) == 0,
                "A hidden chooser launched or unexpectedly resumed its task");
        check(((List<?>)get(hidden.root, "pendingResults")).size() == 1, "Live hidden caller lost cancellation result");
        Harness foreground = new Harness();
        foreground.asOwner(() -> check(foreground.owner.controller.moveActivityTaskToBack(token(foreground.root), true),
                "Could not prepare a background task"));
        check(foreground.start(chooser()) == ActivityManager.START_SUCCESS, "Valid chooser start did not foreground its task");
        foreground.ui.displayed();
        check(foreground.ui.active.getAsBoolean(), "Foregrounded chooser remained inactive");
        foreground.ui.choose(-1); foreground.settled();
        check(get(foreground.root, "state").toString().equals("RESUMED"), "Chooser cancellation did not restore foreground task");
        Harness meta = new Harness();
        Intent sole = chooser().putExtra(Intent.EXTRA_EXCLUDE_COMPONENTS, new ComponentName[]{new ComponentName(FIRST, FIRST + ".Share")});
        Object resolved = make(type("IntentChooser"), new Class<?>[]{PackageManagerService.class, String.class, Intent.class}, meta.pm, OWNER, sole);
        List<?> options = (List<?>)get(resolved, "options");
        Object option = options.get(0);
        ((ActivityInfo)get(option, "info")).applicationInfo.flags |= android.content.pm.ApplicationInfo.FLAG_SUSPENDED;
        Method auto = type("IntentChooser").getDeclaredMethod("shouldAutoLaunch", Intent.class, List.class); auto.setAccessible(true);
        check(!(Boolean)auto.invoke(null, sole, options), "Suspended sole option auto-launched");
        check(!(Boolean)invoke(option, "current", new Class<?>[]{PackageManagerService.class, String.class}, meta.pm, OWNER),
                "Suspended option remained launchable");
        Intent targetIntent = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra("value", "target");
        Intent wrapper = Intent.createChooser(targetIntent, "Wrapper").setDataAndType(Uri.parse("https://invalid.example/wrapper"), "image/png")
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK | Intent.FLAG_GRANT_READ_URI_PERMISSION);
        wrapper.setClipData(ClipData.newRawUri("grant", Uri.parse("content://" + OWNER + ".files/a")));
        Method prepare = type("IntentChooser").getDeclaredMethod("prepare", Intent.class, Intent.class, android.content.pm.ApplicationInfo.class); prepare.setAccessible(true);
        Intent prepared = (Intent)prepare.invoke(null, targetIntent, wrapper, meta.sender.applicationInfo(0));
        check("text/plain".equals(prepared.getType()) && prepared.getData() == null && "target".equals(prepared.getStringExtra("value"))
                && (prepared.getFlags() & Intent.FLAG_ACTIVITY_CLEAR_TASK) == 0 && prepared.getClipData() != null,
                "Chooser wrapper rewrote target data/type/flags or dropped grant ClipData");
    }

    private static final class Receiver extends IIntentReceiver.Stub {
        final CountDownLatch received = new CountDownLatch(1); volatile Intent intent; final AtomicInteger calls = new AtomicInteger();
        @Override public void performReceive(Intent intent, int code, String data, Bundle extras, boolean ordered, boolean sticky, int user) {
            this.intent = intent; calls.incrementAndGet(); received.countDown();
        }
    }
    private static void callback() throws Exception {
        for (int selection : new int[]{1, -1, 0}) {
            Harness h = new Harness(); Receiver receiver = new Receiver();
            Object broadcasts = get(h.am, "mBroadcasts"); String action = "fixture.chooser.CHOSEN";
            h.asOwner(() -> invoke(broadcasts, "registerReceiver", new Class<?>[]{IApplicationThread.class, String.class,
                    IIntentReceiver.class, IntentFilter.class, int.class, String.class, int.class}, h.owner, OWNER, receiver,
                    new IntentFilter(action), PID, null, Context.RECEIVER_NOT_EXPORTED));
            AtomicReference<IIntentSender> sender = new AtomicReference<>();
            h.asOwner(() -> sender.set(h.am.getIntentSenderWithFeature(ActivityManager.INTENT_SENDER_BROADCAST,
                    OWNER, null, null, null, 80, new Intent[]{new Intent(action).setPackage(OWNER)}, new String[]{null},
                    PendingIntent.FLAG_MUTABLE, null, 0)));
            Intent request = chooser().putExtra(Intent.EXTRA_CHOSEN_COMPONENT_INTENT_SENDER, new IntentSender(sender.get()));
            if (selection == 0) {
                Intent target = request.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
                target.setClipData(ClipData.newRawUri("private", Uri.parse("content://" + SECOND + ".files/private")));
                target.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            }
            h.start(request); h.ui.displayed(); check(receiver.calls.get() == 0, "Chosen callback fired before selection");
            h.ui.choose(selection); h.settled();
            if (selection != 1) {
                check(!receiver.received.await(150, TimeUnit.MILLISECONDS) && receiver.calls.get() == 0,
                        "Cancel or rejected URI grant reported a chosen component");
                check(h.launched(FIRST + ".Share") == null && h.launched(SECOND + ".Share") == null,
                        "Failed chooser selection published an activity");
                continue;
            }
            check(receiver.received.await(3, TimeUnit.SECONDS) && receiver.calls.get() == 1, "Chosen PendingIntent was not delivered exactly once");
            check(new ComponentName(SECOND, SECOND + ".Share").equals(receiver.intent.getParcelableExtra(Intent.EXTRA_CHOSEN_COMPONENT, ComponentName.class)),
                    "Callback reported the wrong component");
            check(receiver.intent.getParcelableExtra(Intent.EXTRA_CHOOSER_RESULT, android.service.chooser.ChooserResult.class) != null,
                    "Android 16 chooser result is missing");
            check(h.launched(SECOND + ".Share") != null, "Callback reported a nonexistent launch");
        }
        Harness raced = new Harness();
        set(raced.tasks, "mBeforeActivityPublication", (Runnable)() -> {
            try { set(raced.root, "finishing", true); } catch (Exception e) { throw new AssertionError(e); }
        });
        Uri uri = Uri.parse("content://" + OWNER + ".files/race");
        Intent request = Intent.createChooser(new Intent(Intent.ACTION_SEND).setType("text/plain").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION).putExtra(Intent.EXTRA_STREAM, uri), "Race");
        raced.start(request); raced.ui.displayed(); raced.ui.choose(0); raced.settled();
        check(raced.launched(FIRST + ".Share") == null && !raced.access(raced.target1, uri, 1),
                "Caller retirement during final activity publication escaped the capability check");
    }
    @SuppressWarnings("unchecked")
    private static void idleOwnership() throws Exception {
        for (String mode : List.of("cancel", "no-poll", "open-failure", "exception", "linkage", "process", "caller-code", "accept")) {
            Harness h = new Harness();
            check(h.idleSince() == 0, "Ordinary activity did not keep the device busy");
            h.withoutActivities();
            check(h.idleSince() == 1, "Activity-free fixture was not idle before chooser admission");
            check(h.start(chooser().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), null, -1) == ActivityManager.START_SUCCESS,
                    "Activity-free chooser was not admitted: " + mode);
            h.ui.displayed();
            check(h.records().isEmpty() && h.pending() == 1 && h.ui.active.getAsBoolean(),
                    "Native chooser fabricated an activity or lost its reservation: " + mode);
            check((Long)get(h.tasks, "mIdleSince") == 0, "Chooser admission did not restart the idle clock: " + mode);
            if (!mode.equals("no-poll")) check(h.idleSince() == 0, "Open chooser allowed device idle shutdown: " + mode);
            if (mode.equals("exception")) h.ui.failure = new IllegalStateException("Fixture native window failed");
            if (mode.equals("linkage")) h.ui.failure = new UnsatisfiedLinkError("Fixture native backend disappeared");
            if (mode.equals("process")) invoke(h.tasks, "processDied", new Class<?>[]{h.ownerProcess.getClass()}, h.ownerProcess);
            if (mode.equals("caller-code")) ((Map<String, ApkPackage>)get(h.pm, "mPackages")).put(OWNER, code(OWNER, UID));
            if (mode.equals("process") || mode.equals("caller-code")) {
                check(!h.ui.active.getAsBoolean() && h.idleSince() == 0,
                        "Retired owner released device use before its native window closed: " + mode);
            }
            long closedAfter = SystemClock.uptimeMillis();
            h.ui.choose(mode.equals("open-failure") ? -3 : mode.equals("accept") ? 1 : -1);
            h.settled();
            if (mode.equals("accept")) {
                Object target = h.launched(SECOND + ".Share");
                check(target != null && h.idleSince() == 0, "Accepted chooser lost ordinary activity busy ownership");
                long identity = Binder.clearCallingIdentity();
                try {
                    Binder.restoreCallingIdentity(((long)(UID + 2) << 32) | (PID + 2));
                    h.second.controller.finishActivity(token(target), Activity.RESULT_CANCELED, null, 0);
                } finally { Binder.restoreCallingIdentity(identity); }
            }
            long finishedIdle = (Long)get(h.tasks, "mIdleSince");
            check(h.records().isEmpty() && h.pending() == 0 && finishedIdle >= closedAfter && finishedIdle > 1,
                    "Chooser completion leaked busy ownership or retained its old idle interval: " + mode);
            check(h.idleSince() == finishedIdle && h.owner.results.isEmpty(),
                    "Completed headless chooser restarted idle again or invented an activity result: " + mode);
        }
        Harness unavailable = new Harness(); unavailable.withoutActivities(); unavailable.ui.available = false;
        check(unavailable.start(chooser().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), null, -1)
                == ActivityManager.START_INTENT_NOT_RESOLVED && unavailable.pending() == 0 && unavailable.idleSince() == 1,
                "Unavailable chooser acquired or leaked busy ownership");

        Harness concurrent = new Harness(); concurrent.withoutActivities();
        concurrent.start(chooser().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), null, -1); concurrent.ui.displayed();
        Selection secondUi = new Selection(); set(concurrent.tasks, "mIntentChooserDialog", secondUi.dialog());
        long identity = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long)(UID + 1) << 32) | (PID + 1));
            check(concurrent.api.startActivity(concurrent.first, FIRST, null,
                    chooser().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), null, null, null, -1, 0, null, null)
                    == ActivityManager.START_SUCCESS, "Second live caller's chooser was not admitted");
        } finally { Binder.restoreCallingIdentity(identity); }
        secondUi.displayed(); check(concurrent.pending() == 2, "Concurrent chooser reservations were lost");
        concurrent.ui.choose(-1);
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
        while (concurrent.pending() != 1 && System.nanoTime() < deadline) Thread.sleep(5);
        check(concurrent.pending() == 1 && secondUi.active.getAsBoolean() && concurrent.idleSince() == 0,
                "Closing one chooser released another chooser's device use");
        long closedAfter = SystemClock.uptimeMillis();
        secondUi.choose(-1); concurrent.settled();
        check(concurrent.records().isEmpty() && concurrent.idleSince() >= closedAfter,
                "Last chooser did not start a fresh idle interval");
    }
    private static void detachedOverlap() throws Exception {
        Harness h = new Harness();
        Selection secondUi = new Selection();
        try {
            check(h.start(chooser().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), null, -1)
                    == ActivityManager.START_SUCCESS, "First detached chooser admitted");
            h.ui.displayed();
            set(h.tasks, "mIntentChooserDialog", secondUi.dialog());
            long oldIdentity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)(UID + 1) << 32) | (PID + 1));
                check(h.api.startActivity(h.first, FIRST, null,
                        chooser().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), null, null, null, -1,
                        0, null, null) == ActivityManager.START_SUCCESS, "Second detached caller admitted");
            } finally { Binder.restoreCallingIdentity(oldIdentity); }
            secondUi.displayed();
            check(h.pending() == 2 && h.idleSince() == 0, "Both detached coordinators own native sessions");
            h.ui.choose(-1); h.ui.worker.join(3000);
            check(!h.ui.worker.isAlive() && h.pending() == 1, "First native call and cleanup actually returned");
            System.out.println("DETACHED78 source=" + get(h.root, "state") + " resumes="
                    + h.owner.count(ResumeActivityItem.class) + " remaining=" + h.pending()
                    + " secondActive=" + secondUi.active.getAsBoolean());
            check(get(h.root, "state") == state("PAUSED") && h.owner.count(ResumeActivityItem.class) == 0,
                    "First detached chooser completion must not resume beneath the second detached chooser");
            check(secondUi.active.getAsBoolean() && h.idleSince() == 0 && h.owner.results.isEmpty(),
                    "Remaining chooser retains its native lifetime and creates no result dependency");
            secondUi.choose(-1); h.settled();
            check(get(h.root, "state") == state("RESUMED") && h.owner.count(ResumeActivityItem.class) == 1,
                    "Final detached chooser resumes the covered task once");
        } finally { h.ui.choose(-1); secondUi.choose(-1); h.settled(); }
    }

    public static int run(boolean binder) throws Exception {
        realBinder = binder; checks = 0;
        idleOwnership(); detachedOverlap();
        validation(); parcelledSharePreparation(); selectionAndGrants(); cancellation(); initialsAndSingle(); sharedUid(); selectors(); providerTypes();
        retirement(); flagsAndSuspension(); callback();
        return checks;
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        android.content.res.AssetManager.createSystemAssetsInZygoteLocked(false, System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        System.out.println("ChooserContractTest all passed: " + run(true) + " checks (actual AIDL/Parcel, production launches and grants; scripted user-input boundary)");
    }
}
