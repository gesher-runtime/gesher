/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.accounts.Account;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.content.ComponentName;
import android.content.AbstractThreadedSyncAdapter;
import android.content.AttributionSource;
import android.content.ContentProvider;
import android.content.ContentProviderClient;
import android.content.ContentValues;
import android.content.ContentResolver;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.IContentProvider;
import android.content.ISyncAdapter;
import android.content.ISyncAdapterUnsyncableAccountCallback;
import android.content.ISyncContext;
import android.content.ISyncStatusObserver;
import android.content.Intent;
import android.content.SyncInfo;
import android.content.SyncResult;
import android.content.pm.ServiceInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.Signature;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.database.Cursor;
import android.net.Uri;
import android.service.notification.StatusBarNotification;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.PriorityQueue;
import java.util.concurrent.AbstractExecutorService;
import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Delayed;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiPredicate;
import java.util.function.LongSupplier;
import java.util.function.ToIntBiFunction;
import sun.misc.Unsafe;

/** Production manager and binding registry; constructed package metadata and deterministic scheduling. */
public final class SyncManagerContractTest {
    private static final String PACKAGE = "gesher.sync.contract", AUTHORITY = "gesher.sync.rows", TYPE = "gesher.sync.account";
    private static final int PID = 873101, UID = 12371;
    private static final Account ONE = new Account("one", TYPE), TWO = new Account("two", TYPE);
    private static final Unsafe UNSAFE;
    public static int checks;
    static {
        try { Field field = Unsafe.class.getDeclaredField("theUnsafe"); field.setAccessible(true); UNSAFE = (Unsafe) field.get(null); }
        catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    private static synchronized void check(boolean value, String why) { if (!value) throw new AssertionError(why); checks++; }
    private static Field field(Class<?> type, String name) throws Exception { Field f = type.getDeclaredField(name); f.setAccessible(true); return f; }
    private static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }
    private static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Object rethrow(Throwable failure) throws Exception {
        if (failure instanceof Error error) throw error;
        if (failure instanceof Exception exception) throw exception;
        throw new AssertionError(failure);
    }
    private static Object make(Class<?> type, Class<?>[] parameters, Object... arguments) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(parameters); c.setAccessible(true);
        try { return c.newInstance(arguments); } catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static Object invoke(Object target, String name, Class<?>[] parameters, Object... arguments) throws Exception {
        Method m = target.getClass().getDeclaredMethod(name, parameters); m.setAccessible(true);
        try { return m.invoke(target, arguments); } catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static IBinder relay(IBinder target) {
        return new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, data, reply, flags);
            }
        };
    }
    private static Bundle extras(String tag) { Bundle b = new Bundle(); b.putString("tag", tag); return b; }
    private static void await(CountDownLatch latch, String name) throws Exception { check(latch.await(3, TimeUnit.SECONDS), name + " timed out"); }

    private static final class Manual extends AbstractExecutorService implements ScheduledExecutorService {
        volatile long now = 10000, wall = 1700000000000L;
        long sequence;
        boolean stopped;
        final PriorityQueue<Task<?>> queue = new PriorityQueue<>();
        final class Task<V> extends FutureTask<V> implements ScheduledFuture<V> {
            final long due, order;
            final AtomicReference<Throwable> thrown;
            Task(Callable<V> work, long delay) { this(work, delay, new AtomicReference<>()); }
            Task(Callable<V> work, long delay, AtomicReference<Throwable> failure) {
                super(() -> { try { return work.call(); } catch (Throwable error) { failure.set(error); throw error; } });
                thrown = failure; due = now + Math.max(0, delay); order = sequence++;
            }
            @Override public long getDelay(TimeUnit unit) { return unit.convert(due - now, TimeUnit.MILLISECONDS); }
            @Override public int compareTo(Delayed other) {
                Task<?> task = (Task<?>) other; int cmp = Long.compare(due, task.due); return cmp == 0 ? Long.compare(order, task.order) : cmp;
            }
        }
        @Override public synchronized <V> ScheduledFuture<V> schedule(Callable<V> work, long delay, TimeUnit unit) {
            Task<V> task = new Task<>(work, unit.toMillis(delay)); queue.add(task); return task;
        }
        @Override public ScheduledFuture<?> schedule(Runnable work, long delay, TimeUnit unit) {
            return schedule(() -> { work.run(); return null; }, delay, unit);
        }
        @Override public void execute(Runnable work) { schedule(work, 0, TimeUnit.MILLISECONDS); }
        @Override public ScheduledFuture<?> scheduleAtFixedRate(Runnable work, long initial, long period, TimeUnit unit) { throw new AssertionError("Fixture does not install periodic syncs"); }
        @Override public ScheduledFuture<?> scheduleWithFixedDelay(Runnable work, long initial, long delay, TimeUnit unit) { throw new AssertionError("Unexpected fixed delay"); }
        void drain() throws Exception {
            int runs = 0;
            for (;;) {
                Task<?> task;
                synchronized (this) {
                    task = queue.peek();
                    if (task == null || task.due > now) return;
                    queue.remove();
                }
                if (task.isCancelled()) continue;
                if (++runs > 1000) throw new AssertionError("Unbounded immediate retry loop");
                task.run();
                if (task.thrown.get() != null) rethrow(task.thrown.get());
                // A wakeup can retire its own ScheduledFuture while its Runnable runs.
                if (task.isCancelled()) continue;
                try { task.get(); } catch (ExecutionException failure) { rethrow(failure.getCause()); }
            }
        }
        @Override public synchronized void shutdown() { stopped = true; queue.clear(); }
        @Override public List<Runnable> shutdownNow() { shutdown(); return List.of(); }
        @Override public boolean isShutdown() { return stopped; }
        @Override public boolean isTerminated() { return stopped; }
        @Override public boolean awaitTermination(long time, TimeUnit unit) { return stopped; }
    }

    private record Start(ISyncContext context, Account account, Bundle extras) { }
    private static final class Harness implements AutoCloseable {
        final boolean wire;
        final Manual timer = new Manual();
        final Object pm, atm, services, notifications, sync, adapter, previousNotifications;
        final IActivityManager am;
        final Class<?> adapterType = type("SyncManager$Adapter");
        final List<Start> starts = new ArrayList<>();
        int cancelled, unbound;
        IBinder suppliedAdapter;
        volatile CountDownLatch blockCreate, createEntered, blockStart, startEntered;
        final ISyncAdapter endpoint = new ISyncAdapter.Stub() {
            @Override public void onUnsyncableAccount(ISyncAdapterUnsyncableAccountCallback callback) { }
            @Override public void startSync(ISyncContext context, String authority, Account account, Bundle extras) {
                check(!Thread.holdsLock(sync), "startSync callback held the registry monitor");
                ISyncContext completion = wire ? ISyncContext.Stub.asInterface(relay(context.asBinder())) : context;
                starts.add(new Start(completion, account, new Bundle(extras)));
                if (startEntered != null) startEntered.countDown();
                if (blockStart != null) {
                    try { if (!blockStart.await(5, TimeUnit.SECONDS)) throw new AssertionError("Held start callback timed out"); }
                    catch (InterruptedException failure) { throw new AssertionError(failure); }
                }
            }
            @Override public void cancelSync(ISyncContext context) { cancelled++; }
        };
        final IApplicationThread thread = new IApplicationThread.Default() {
            final IBinder binder = new Binder();
            @Override public IBinder asBinder() { return binder; }
            @Override public void scheduleCreateService(IBinder token, ServiceInfo info, CompatibilityInfo compat, int state) {
                check(!Thread.holdsLock(sync), "bind callback held the registry monitor");
                if (createEntered != null) createEntered.countDown();
                if (blockCreate != null) {
                    try { if (!blockCreate.await(5, TimeUnit.SECONDS)) throw new AssertionError("Held bind callback timed out"); }
                    catch (InterruptedException failure) { throw new AssertionError(failure); }
                }
            }
            @Override public void scheduleBindService(IBinder token, IBinder bindToken, Intent intent, boolean rebind, int state, long sequence) {
                IBinder published = suppliedAdapter == null ? endpoint.asBinder() : suppliedAdapter;
                try { invoke(services, "publishService", new Class<?>[]{IBinder.class, IBinder.class, IBinder.class},
                        token, bindToken, wire ? relay(published) : published); }
                catch (Exception failure) { throw new AssertionError(failure); }
            }
            @Override public void scheduleUnbindService(IBinder token, IBinder bindToken, Intent intent) { unbound++; }
            @Override public void scheduleStopService(IBinder token) { }
        };
        @SuppressWarnings("unchecked") Harness(boolean parallel, boolean wire) throws Exception {
            this.wire = wire;
            PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(PACKAGE, "/fixture/" + PACKAGE + "/base.apk");
            ParsedServiceImpl service = new ParsedServiceImpl();
            service.setName(PACKAGE + ".Adapter"); service.setPackageName(PACKAGE); service.setProcessName(PACKAGE); service.setEnabled(true);
            parsed.addService(service); parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.hideAsFinal();
            Object owner = UNSAFE.allocateInstance(type("ApkPackage"));
            set(owner, "packageName", PACKAGE); set(owner, "mUid", UID); set(owner, "parsed", parsed);
            set(owner, "apkPath", "/fixture/" + PACKAGE + "/base.apk"); set(owner, "mDataDir", "/fixture/" + PACKAGE + "/data");
            set(owner, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
            set(owner, "mSigningIdentityChecked", true); set(owner, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{7})});
            Class<?> pmType = type("PackageManagerService"), atmType = type("ActivityTaskManagerService"),
                    amType = type("ActivityManagerService"), callersType = type("CallerIdentity"), notificationType = type("NotificationManagerService");
            pm = make(pmType, new Class<?>[]{Collection.class}, List.of(owner));
            atm = make(atmType, new Class<?>[]{pmType}, pm);
            // Isolate unrelated providers/UI startup. Called binding/identity methods stay real.
            am = (IActivityManager) UNSAFE.allocateInstance(amType);
            Object callers = make(callersType, new Class<?>[]{pmType, atmType}, pm, atm);
            set(am, "mPm", pm); set(am, "mAtm", atm); set(am, "mCallers", callers);
            Object permissions = invoke(pm, "permissions", new Class<?>[0]);
            set(am, "mPermissions", permissions); invoke(permissions, "attach", new Class<?>[]{callersType}, callers);
            Object catalog = get(permissions, "mCatalog"); set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            previousNotifications = field(notificationType, "sLast").get(null);
            ToIntBiFunction<StatusBarNotification, Boolean> toast = (value, update) -> 0;
            notifications = make(notificationType, new Class<?>[]{pmType, amType, ToIntBiFunction.class}, pm, am, toast);
            services = make(type("ActiveServices"), new Class<?>[]{atmType, pmType, notificationType}, atm, pm, notifications);
            set(am, "mServices", services); invoke(atm, "setActivityManager", new Class<?>[]{amType}, am);
            Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
            Object process = make(processType, new Class<?>[]{atmType, int.class, IApplicationThread.class}, atm, PID, thread);
            set(process, "uid", UID); set(process, "packageName", PACKAGE); set(process, "processName", PACKAGE); set(process, "ready", true);
            ((List<Object>) get(atm, "mProcesses")).add(process);
            ((List<Object>) get(process, "activities")).add(UNSAFE.allocateInstance(type("ActivityTaskManagerService$ActivityRecord")));
            Class<?> managerType = type("SyncManager"); Object manager;
            try { manager = make(managerType, new Class<?>[]{amType, pmType, LongSupplier.class, LongSupplier.class, ScheduledExecutorService.class},
                    am, pm, (LongSupplier) () -> timer.now, (LongSupplier) () -> timer.wall, timer); }
            catch (NoSuchMethodException baseline) {
                manager = make(managerType, new Class<?>[]{amType, pmType}, am, pm);
                ((ExecutorService) get(manager, "mTimer")).shutdownNow(); set(manager, "mTimer", timer);
            }
            sync = manager;
            set(sync, "mAccountAccess", (BiPredicate<Account, String>) (account, pkg) -> true);
            adapter = adapter(AUTHORITY, TYPE, parallel);
        }
        Object adapter(String authority, String type, boolean parallel) throws Exception {
            return make(adapterType, new Class<?>[]{String.class, String.class, ComponentName.class,
                    boolean.class, boolean.class, boolean.class, boolean.class, String.class},
                    authority, type, new ComponentName(PACKAGE, PACKAGE + ".Adapter"), true, true, true, parallel, null);
        }
        void enqueue(Account account, Bundle extras) throws Exception { enqueue(adapter, account, extras); }
        void enqueue(Object adapter, Account account, Bundle extras) throws Exception {
            invoke(sync, "enqueue", new Class<?>[]{adapterType, Account.class, Bundle.class}, adapter, account, extras);
        }
        void cancel(Account account, String authority) throws Exception { invoke(sync, "cancelSync", new Class<?>[]{Account.class, String.class}, account, authority); }
        int pending() throws Exception { Object p = get(sync, "mPending"); return p instanceof Map<?, ?> map ? map.size() : ((Collection<?>) p).size(); }
        int active() throws Exception { return ((Map<?, ?>) get(sync, "mActive")).size(); }
        int bindings() throws Exception { return ((Map<?, ?>) get(services, "mConnections")).size(); }
        Object firstActive() throws Exception { return ((Map<?, ?>) get(sync, "mActive")).values().iterator().next(); }
        void finish(int index, SyncResult result) throws Exception { starts.get(index).context.onFinished(result); timer.drain(); }
        void advance(long millis) throws Exception { timer.now += millis; timer.drain(); }
        @Override public void close() throws Exception {
            blockCreate = null; blockStart = null;
            cancel(null, null); timer.drain(); timer.shutdownNow();
            try { ((Collection<Object>) field(type("DeviceWork"), "sSources").get(null)).remove(get(sync, "mWorkSource")); }
            catch (NoSuchFieldException baseline) { }
            field(type("NotificationManagerService"), "sLast").set(null, previousNotifications);
            ((ExecutorService) get(services, "mRestarts")).shutdownNow();
            ((ExecutorService) get(pm, "mPackageEventExecutor")).shutdownNow();
        }
    }

    private static void admission(boolean wire) throws Exception {
        try (Harness h = new Harness(false, wire)) {
            h.enqueue(ONE, extras("first")); h.enqueue(TWO, extras("second")); h.timer.drain();
            check(h.starts.size() == 1 && h.active() == 1 && h.pending() == 1, "Nonparallel adapter started two accounts");
            check(h.bindings() == 1, "First sync owns one binding");
            h.enqueue(ONE, extras("again")); h.timer.drain();
            check(h.starts.size() == 1 && h.pending() == 2, "Busy adapter lost queued work");
            h.finish(0, new SyncResult());
            check(h.starts.size() == 2 && h.starts.get(1).account.equals(TWO), "Queued account not promoted");
            h.finish(1, new SyncResult());
            check(h.starts.size() == 3 && "again".equals(h.starts.get(2).extras.getString("tag")), "Same-account rerun lost extras");
            h.finish(2, new SyncResult());
            check(h.active() == 0 && h.pending() == 0 && h.bindings() == 0, "Finished serial syncs retained state");
        }
        try (Harness h = new Harness(true, wire)) {
            h.enqueue(ONE, extras("one")); h.enqueue(TWO, extras("two")); h.timer.drain();
            check(h.starts.size() == 2 && h.active() == 2 && h.bindings() == 2, "Parallel accounts were serialized");
            h.enqueue(ONE, extras("next")); h.timer.drain();
            check(h.starts.size() == 2 && h.pending() == 1, "Same account ran in parallel");
            h.finish(0, new SyncResult());
            check(h.starts.size() == 3 && h.active() == 2, "Parallel same-account work was lost");
        }
        try (Harness h = new Harness(false, wire)) {
            h.enqueue(ONE, extras("one"));
            h.enqueue(h.adapter(AUTHORITY + ".other", TYPE, false), TWO, extras("other")); h.timer.drain();
            check(h.starts.size() == 2, "Independent authorities incorrectly serialized");
        }
        try (Harness h = new Harness(false, wire)) {
            h.enqueue(h.adapter(AUTHORITY, "a", false), new Account("b/c", "a"), extras("left"));
            h.enqueue(h.adapter(AUTHORITY, "a/b", false), new Account("c", "a/b"), extras("right")); h.timer.drain();
            check(h.starts.size() == 2, "Delimiter-containing endpoint identities aliased");
        }
    }

    @SuppressWarnings("unchecked") private static void clock(boolean wire) throws Exception {
        try (Harness h = new Harness(false, wire)) {
            h.enqueue(ONE, extras("clock")); h.timer.drain();
            List<SyncInfo> info = (List<SyncInfo>) invoke(h.sync, "getCurrentSyncs", new Class<?>[0]);
            check(info.size() == 1 && info.get(0).startTime == h.timer.now, "SyncInfo start must use elapsedRealtime");
            h.timer.now += 1000; h.timer.wall -= 5000000; h.starts.get(0).context.sendHeartbeat();
            check(((Long) get(h.firstActive(), "heartbeat")) == h.timer.now, "Heartbeat must use elapsedRealtime");
            check((Long) invoke(h.sync, "nextDueElapsed", new Class<?>[0]) == 0, "Active sync must keep idle device alive");
            check(com.gesher.server.DeviceWork.dueWithin(0), "Actual DeviceWork source missed active sync");
            h.advance(TimeUnit.MINUTES.toMillis(30));
            check(h.active() == 0 && h.cancelled == 1 && h.bindings() == 0, "Elapsed heartbeat timeout failed");
            check((Long) invoke(h.sync, "nextDueElapsed", new Class<?>[0]) == Long.MAX_VALUE, "Idle manager advertises no immediate work");
        }
    }

    private static SyncResult result(String kind) {
        if (kind.equals("already")) return SyncResult.ALREADY_IN_PROGRESS;
        SyncResult r = new SyncResult();
        switch (kind) {
            case "soft": r.stats.numIoExceptions = 1; break;
            case "hard": r.stats.numAuthExceptions = 1; break;
            case "parse": r.stats.numParseExceptions = 1; break;
            case "progress": r.stats.numAuthExceptions = 1; r.stats.numInserts = 1; break;
            case "limit": r.stats.numIoExceptions = 1; r.tooManyRetries = true; break;
            case "database": r.databaseError = true; break;
            default: throw new AssertionError(kind);
        }
        return r;
    }
    private static void results(boolean wire) throws Exception {
        for (String kind : new String[]{"soft", "hard", "parse", "progress", "limit", "database", "already"}) {
            for (int flags = 0; flags < 4; flags++) {
                boolean upload = (flags & 1) != 0, noRetry = (flags & 2) != 0;
                boolean retry = kind.equals("already") || !noRetry && (upload || kind.equals("soft") || kind.equals("progress"));
                try (Harness h = new Harness(false, wire)) {
                    Bundle request = extras(kind); request.putBoolean(ContentResolver.SYNC_EXTRAS_UPLOAD, upload);
                    request.putBoolean(ContentResolver.SYNC_EXTRAS_DO_NOT_RETRY, noRetry);
                    request.putBoolean(ContentResolver.SYNC_EXTRAS_IGNORE_BACKOFF, true);
                    h.enqueue(ONE, request); h.timer.drain(); h.finish(0, result(kind));
                    check(h.pending() == (retry ? 1 : 0), "Result retry decision " + kind + " flags=" + flags);
                    check(h.active() == 0 && h.bindings() == 0, "Completed adapter retained binding " + kind);
                    check(h.starts.size() == 1, "Backoff was bypassed for retry " + kind);
                    if (retry) {
                        h.advance(60000);
                        check(h.starts.size() == 2, "Due retry never started " + kind);
                        Bundle replay = h.starts.get(1).extras;
                        check(!replay.containsKey(ContentResolver.SYNC_EXTRAS_IGNORE_BACKOFF), "Retry retained ignore-backoff");
                        check(replay.getBoolean(ContentResolver.SYNC_EXTRAS_UPLOAD) == (upload && kind.equals("already")), "Upload retry must become two-way");
                        check(kind.equals(replay.getString("tag")), "Retry lost application extras");
                    }
                }
            }
        }
        try (Harness h = new Harness(false, wire)) {
            h.enqueue(ONE, extras("success-clears")); h.timer.drain(); h.finish(0, result("soft"));
            h.advance(60000); h.finish(1, new SyncResult());
            h.enqueue(ONE, extras("fresh")); h.timer.drain();
            check(h.starts.size() == 3, "Success did not clear endpoint backoff");
        }
        try (Harness h = new Harness(false, wire)) {
            Bundle request = extras("original"); request.putBoolean(ContentResolver.SYNC_EXTRAS_DO_NOT_RETRY, true);
            h.enqueue(ONE, request); h.timer.drain();
            SyncResult full = new SyncResult(); full.fullSyncRequested = true; h.finish(0, full);
            check(h.starts.size() == 2 && h.starts.get(1).extras.isEmpty(), "Full sync must use new empty extras");
        }
        try (Harness h = new Harness(false, wire)) {
            h.enqueue(ONE, extras("retry")); h.timer.drain();
            SyncResult full = result("soft"); full.fullSyncRequested = true; h.finish(0, full);
            check(h.pending() == 2, "Retry and full sync were merged");
            h.advance(60000); check(h.starts.size() == 2, "Retry admission after backoff");
            h.finish(1, new SyncResult());
            check(h.starts.size() == 3 && h.starts.get(2).extras.isEmpty(), "Independent full sync lost");
        }
        try (Harness h = new Harness(false, wire)) {
            h.enqueue(ONE, extras("delay")); h.timer.drain();
            SyncResult later = new SyncResult(); later.delayUntil = h.timer.wall / 1000 + 120;
            h.finish(0, later); h.enqueue(ONE, extras("fresh")); h.timer.drain();
            check(h.starts.size() == 1 && h.pending() == 1, "delayUntil was ignored");
            h.timer.wall += 10000000; h.advance(119999);
            check(h.starts.size() == 1, "Wall-clock change moved monotonic delay deadline");
            h.advance(1); check(h.starts.size() == 2, "delayUntil did not release at deadline");
            SyncResult negative = new SyncResult(); negative.delayUntil = Long.MIN_VALUE;
            h.finish(1, negative); h.enqueue(ONE, extras("after-negative")); h.timer.drain();
            check(h.starts.size() == 3, "Negative delayUntil overflowed into future");
        }
    }

    /** Real AbstractThreadedSyncAdapter owns both worker threads and provider clients on ART. */
    private static void threaded() throws Exception {
        for (boolean parallel : new boolean[]{false, true}) {
            try (Harness h = new Harness(parallel, true)) {
                CountDownLatch one = new CountDownLatch(1), two = new CountDownLatch(1);
                CountDownLatch releaseOne = new CountDownLatch(1), releaseTwo = new CountDownLatch(1);
                AtomicInteger running = new AtomicInteger(), maximum = new AtomicInteger(), released = new AtomicInteger();
                AtomicReference<Throwable> failure = new AtomicReference<>();
                ContentProvider provider = new ContentProvider() {
                    @Override public boolean onCreate() { return true; }
                    @Override public Cursor query(Uri u, String[] p, String s, String[] a, String o) { throw new AssertionError("Unused provider query"); }
                    @Override public String getType(Uri u) { return null; }
                    @Override public Uri insert(Uri u, ContentValues v) { throw new AssertionError(); }
                    @Override public int delete(Uri u, String s, String[] a) { throw new AssertionError(); }
                    @Override public int update(Uri u, ContentValues v, String s, String[] a) { throw new AssertionError(); }
                };
                class TestContext extends ContextWrapper {
                    ContentResolver resolver;
                    TestContext() { super(null); }
                    @Override public String getPackageName() { return PACKAGE; }
                    @Override public String getOpPackageName() { return PACKAGE; }
                    @Override public ApplicationInfo getApplicationInfo() { return new ApplicationInfo(); }
                    @Override public AttributionSource getAttributionSource() { return new AttributionSource.Builder(android.os.Process.myUid()).setPackageName(PACKAGE).build(); }
                    @Override public ContentResolver getContentResolver() { return resolver; }
                }
                TestContext context = new TestContext();
                context.resolver = new ContentResolver(context) {
                    @Override protected IContentProvider acquireProvider(Context c, String name) { return provider.getIContentProvider(); }
                    @Override protected IContentProvider acquireUnstableProvider(Context c, String name) { return provider.getIContentProvider(); }
                    @Override public boolean releaseProvider(IContentProvider p) { released.incrementAndGet(); return true; }
                    @Override public boolean releaseUnstableProvider(IContentProvider p) { throw new AssertionError(); }
                    @Override public void unstableProviderDied(IContentProvider p) { throw new AssertionError(); }
                };
                AbstractThreadedSyncAdapter real = new AbstractThreadedSyncAdapter(context, false, parallel) {
                    @Override public void onPerformSync(Account account, Bundle extras, String authority,
                            ContentProviderClient client, SyncResult result) {
                        int active = running.incrementAndGet(); maximum.accumulateAndGet(active, Math::max);
                        try {
                            check(client != null, "Real threaded adapter did not acquire provider client");
                            (account.equals(ONE) ? one : two).countDown();
                            if (!(account.equals(ONE) ? releaseOne : releaseTwo).await(5, TimeUnit.SECONDS)) {
                                throw new AssertionError("Threaded adapter release timed out");
                            }
                        } catch (Throwable error) { failure.compareAndSet(null, error); result.databaseError = true; }
                        finally { running.decrementAndGet(); }
                    }
                };
                h.suppliedAdapter = real.getSyncAdapterBinder();
                try {
                    h.enqueue(ONE, extras("one")); h.enqueue(TWO, extras("two")); h.timer.drain();
                    await(one, "First real sync worker");
                    if (parallel) await(two, "Second parallel sync worker");
                    else check(two.getCount() == 1 && h.pending() == 1, "Serial real adapter overlapped accounts");
                    check(com.gesher.server.DeviceWork.dueWithin(0), "Real active sync is missing from device idle accounting");
                    releaseOne.countDown();
                    long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                    while (two.getCount() != 0 && System.nanoTime() < deadline) { h.timer.drain(); Thread.sleep(5); }
                    check(two.getCount() == 0, "Queued real adapter account never started");
                    releaseTwo.countDown();
                    deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                    while ((h.active() != 0 || running.get() != 0 || released.get() != 2) && System.nanoTime() < deadline) {
                        h.timer.drain(); Thread.sleep(5);
                    }
                    check(failure.get() == null, "Real sync worker failed: " + failure.get());
                    check(maximum.get() == (parallel ? 2 : 1), "Real worker concurrency does not match adapter declaration");
                    check(h.active() == 0 && h.bindings() == 0 && released.get() == 2, "Real worker/provider/binding lifetime leaked");
                } finally { releaseOne.countDown(); releaseTwo.countDown(); }
            }
        }
    }

    private static void blockedCancel(boolean wire, boolean duringStart) throws Exception {
        try (Harness h = new Harness(false, wire)) {
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            if (duringStart) { h.startEntered = entered; h.blockStart = release; }
            else { h.createEntered = entered; h.blockCreate = release; }
            h.enqueue(ONE, extras("held"));
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread worker = new Thread(() -> { try { h.timer.drain(); } catch (Throwable error) { failure.set(error); } });
            worker.setDaemon(true); worker.start();
            try {
                await(entered, "External callback");
                CountDownLatch returned = new CountDownLatch(1);
                Thread cancel = new Thread(() -> { try { h.cancel(ONE, AUTHORITY); } catch (Throwable error) { failure.set(error); } finally { returned.countDown(); } });
                cancel.setDaemon(true); cancel.start();
                check(returned.await(1, TimeUnit.SECONDS), "Cancellation waited for external callback");
            } finally { release.countDown(); worker.join(3000); }
            check(!worker.isAlive() && failure.get() == null, "Callback/cancellation did not finish: " + failure.get());
            h.timer.drain();
            check(h.active() == 0 && h.pending() == 0 && h.bindings() == 0, "Cancellation leaked late binding");
            check(h.starts.size() == (duringStart ? 1 : 0), "Retired operation started after cancellation");
            if (duringStart) {
                h.starts.get(0).context.onFinished(result("soft")); h.timer.drain();
                check(h.cancelled == 1 && h.pending() == 0, "Late completion retried or cancelled twice");
            }
        }
    }
    private static void cancellation(boolean wire) throws Exception {
        try (Harness h = new Harness(false, wire)) {
            AtomicBoolean once = new AtomicBoolean();
            ISyncStatusObserver observer = new ISyncStatusObserver.Stub() {
                @Override public void onStatusChanged(int which) {
                    if ((which & ContentResolver.SYNC_OBSERVER_TYPE_ACTIVE) != 0 && once.compareAndSet(false, true)) {
                        try { h.cancel(ONE, AUTHORITY); } catch (Exception error) { throw new AssertionError(error); }
                    }
                }
            };
            invoke(h.sync, "addStatusChangeListener", new Class<?>[]{int.class, ISyncStatusObserver.class}, ContentResolver.SYNC_OBSERVER_TYPE_ACTIVE, observer);
            h.enqueue(ONE, extras("cancel-before-bind")); h.timer.drain();
            check(h.starts.isEmpty() && h.active() == 0 && h.bindings() == 0, "Cancelled admission created a late binding");
        }
        blockedCancel(wire, false); blockedCancel(wire, true);
        for (String kind : new String[]{"soft", "hard"}) {
            try (Harness h = new Harness(false, wire)) {
                h.enqueue(ONE, extras(kind)); h.timer.drain(); h.finish(0, result(kind));
                h.cancel(ONE, AUTHORITY); h.timer.drain(); h.enqueue(ONE, extras("fresh")); h.timer.drain();
                check(h.starts.size() == 2, "Cancellation must reset " + kind + " endpoint backoff");
            }
        }
        try (Harness h = new Harness(true, wire)) {
            h.enqueue(ONE, extras("one")); h.enqueue(TWO, extras("two")); h.timer.drain();
            h.finish(0, result("hard")); h.finish(1, result("hard"));
            h.cancel(ONE, AUTHORITY); h.timer.drain();
            h.enqueue(ONE, extras("one-fresh")); h.enqueue(TWO, extras("two-fresh")); h.timer.drain();
            check(h.starts.size() == 3 && h.starts.get(2).account.equals(ONE) && h.pending() == 1, "Cancellation reset another endpoint backoff");
        }
        try (Harness h = new Harness(false, wire)) {
            h.enqueue(ONE, extras("delay")); h.timer.drain();
            SyncResult delayed = result("soft"); delayed.delayUntil = h.timer.wall / 1000 + 120;
            h.finish(0, delayed); h.cancel(ONE, AUTHORITY); h.timer.drain();
            h.enqueue(ONE, extras("fresh")); h.timer.drain();
            check(h.starts.size() == 1 && h.pending() == 1, "Cancellation cleared adapter delayUntil");
            h.advance(120000); check(h.starts.size() == 2, "Preserved delayUntil never released");
        }
    }

    public static void contracts(String group, boolean wire) throws Exception {
        if (group.equals("all") || group.equals("admission")) admission(wire);
        if (group.equals("all") || group.equals("clock")) clock(wire);
        if (group.equals("all") || group.equals("results")) results(wire);
        if (group.equals("all") || group.equals("cancellation")) cancellation(wire);
        if (wire && group.equals("all")) threaded();
    }
    public static void main(String[] args) throws Exception {
        contracts(args.length == 0 ? "all" : args[0], true);
        System.out.println("Sync manager contracts passed: " + checks + " checks; actual in-process AIDL/Parcel and service bindings");
    }
}
