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
import com.android.internal.pm.pkg.component.ParsedIntentInfoImpl;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.PriorityQueue;
import java.util.Properties;
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
import java.util.function.Supplier;
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
        final List<Runnable> periodicDeliveries = new ArrayList<>();
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
        @Override public ScheduledFuture<?> scheduleAtFixedRate(Runnable work, long initial, long period, TimeUnit unit) {
            // Later periodic delivery is controlled explicitly by the retirement race fixture.
            periodicDeliveries.add(work); return schedule(work, initial, unit);
        }
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
        final Map<Account, Object> identities = new java.util.HashMap<>();
        int cancelled, unbound;
        IBinder suppliedAdapter;
        volatile CountDownLatch blockCreate, createEntered, blockStart, startEntered;
        Runnable duringCreate;
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
                if (duringCreate != null) duringCreate.run();
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
        Harness(boolean parallel, boolean wire) throws Exception { this(parallel, wire, false); }
        @SuppressWarnings("unchecked") Harness(boolean parallel, boolean wire, boolean installed) throws Exception {
            this.wire = wire;
            String apk = "/fixture/" + PACKAGE + "/base.apk", data = "/fixture/" + PACKAGE + "/data";
            if (installed) {
                java.nio.file.Path root = java.nio.file.Path.of(System.getenv("ANDROID_DATA"));
                java.nio.file.Path code = root.resolve("app/" + PACKAGE + "-1");
                java.nio.file.Files.createDirectories(code);
                java.nio.file.Files.writeString(code.resolve("base.apk"), "Synthetic metadata; never parsed as an APK");
                apk = code.resolve("base.apk").toString(); data = root.resolve("data/" + PACKAGE).toString();
                java.nio.file.Files.createDirectories(java.nio.file.Path.of(data));
            }
            PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(PACKAGE, apk);
            ParsedServiceImpl service = new ParsedServiceImpl();
            service.setName(PACKAGE + ".Adapter"); service.setPackageName(PACKAGE); service.setProcessName(PACKAGE); service.setEnabled(true);
            ParsedIntentInfoImpl filter = new ParsedIntentInfoImpl();
            filter.getIntentFilter().addAction("android.content.SyncAdapter"); service.addIntent(filter);
            Bundle metadata = new Bundle(); metadata.putInt("android.content.SyncAdapter", 1);
            service.setMetaData(metadata);
            parsed.addService(service); parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.hideAsFinal();
            Object owner = UNSAFE.allocateInstance(type("ApkPackage"));
            set(owner, "packageName", PACKAGE); set(owner, "mUid", UID); set(owner, "parsed", parsed);
            set(owner, "apkPath", apk); set(owner, "mDataDir", data);
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
            // Unbinding also revokes the connection's URI grants. Keep that
            // production policy object while isolating unrelated provider startup.
            Object providers = UNSAFE.allocateInstance(type("ContentProviders"));
            set(providers, "mUriGrants", make(type("UriGrants"), new Class<?>[]{pmType, atmType}, pm, atm));
            set(am, "mProviders", providers);
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
            set(sync, "mAccounts", (Supplier<Account[]>) () -> new Account[]{ONE, TWO});
            identities.put(ONE, new Object()); identities.put(TWO, new Object());
            try { set(sync, "mAccountIdentity", (java.util.function.Function<Account, Object>) identities::get); }
            catch (NoSuchFieldException baseline) { }
            adapter = adapter(AUTHORITY, TYPE, parallel);
            // Supply parsed adapter XML at the existing package-metadata boundary.
            ((Map<ComponentName, Object[]>) get(sync, "mParsed")).put(
                    new ComponentName(PACKAGE, PACKAGE + ".Adapter"),
                    new Object[]{apk, adapter});
        }
        Object adapter(String authority, String type, boolean parallel) throws Exception {
            return make(adapterType, new Class<?>[]{String.class, String.class, ComponentName.class,
                    boolean.class, boolean.class, boolean.class, boolean.class, String.class},
                    authority, type, new ComponentName(PACKAGE, PACKAGE + ".Adapter"), true, true, true, parallel, null);
        }
        void enqueue(Account account, Bundle extras) throws Exception { enqueue(adapter, account, extras); }
        void enqueue(Object adapter, Account account, Bundle extras) throws Exception {
            Object identity = identities.computeIfAbsent(account, unused -> new Object());
            try { invoke(sync, "enqueue", new Class<?>[]{adapterType, Account.class, Bundle.class, Object.class}, adapter, account, extras, identity); }
            catch (NoSuchMethodException baseline) {
                invoke(sync, "enqueue", new Class<?>[]{adapterType, Account.class, Bundle.class}, adapter, account, extras);
            }
        }
        void cancel(Account account, String authority) throws Exception { invoke(sync, "cancelSync", new Class<?>[]{Account.class, String.class}, account, authority); }
        void setting(String name, String value) throws Exception {
            for (String prefix : new String[]{"auto.", "syncable."}) {
                String legacy = prefix + TYPE + "/one/";
                if (name.startsWith(legacy)) {
                    Method id = type("SyncManager").getDeclaredMethod("id", Account.class, String.class);
                    id.setAccessible(true); name = prefix + id.invoke(null, ONE, name.substring(legacy.length()));
                }
            }
            synchronized (sync) { ((Properties) get(sync, "mSettings")).setProperty(name, value); }
        }
        void request(Bundle extras) throws Exception {
            check(((List<?>) invoke(sync, "adapters", new Class<?>[0])).size() == 1,
                    "Request fixture must resolve the actual sync adapter");
            invoke(sync, "requestSync", new Class<?>[]{Account.class, String.class, Bundle.class, boolean.class},
                    ONE, AUTHORITY, extras, false);
        }
        int pending() throws Exception { Object p = get(sync, "mPending"); return p instanceof Map<?, ?> map ? map.size() : ((Collection<?>) p).size(); }
        int active() throws Exception { return ((Map<?, ?>) get(sync, "mActive")).size(); }
        int bindings() throws Exception { return ((Map<?, ?>) get(services, "mConnections")).size(); }
        Object firstActive() throws Exception { return ((Map<?, ?>) get(sync, "mActive")).values().iterator().next(); }
        void finish(int index, SyncResult result) throws Exception { starts.get(index).context.onFinished(result); timer.drain(); }
        void advance(long millis) throws Exception { timer.now += millis; timer.drain(); }
        void retire(String packageName, java.util.Set<Account> removed) throws Exception {
            Class<?> storageType = type("PackageRemovalStorage");
            Object storage = make(storageType, new Class<?>[]{String.class, String.class}, packageName,
                    new java.io.File(System.getenv("ANDROID_DATA"), "app/" + packageName + "-1").getAbsolutePath());
            Runnable finish;
            synchronized (sync) {
                Object prepared = invoke(sync, "prepareRemoval", new Class<?>[]{String.class, java.util.Set.class, storageType}, packageName, removed, storage);
                identities.keySet().removeAll(removed);
                finish = (Runnable) invoke(sync, "publishRemoval", new Class<?>[]{type("SyncManager$Removal")}, prepared);
                invoke(storage, "publishReplacements", new Class<?>[0]);
            }
            finish.run();
        }
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

    /** The same binding harness, with actual AccountManager authorization instead
     * of its general scheduling fixture's permissive account predicate. */
    private static final class AccountAccessHarness implements AutoCloseable {
        final Harness h;
        final Object accounts, owner;
        final Map<Account, Object> entries;
        final AtomicInteger accessChecks = new AtomicInteger();
        int retireAtAccess;
        @SuppressWarnings("unchecked") AccountAccessHarness(boolean wire) throws Exception {
            h = new Harness(false, wire, true);
            owner = invoke(h.pm, "get", new Class<?>[]{String.class}, PACKAGE);
            accounts = make(type("AccountManagerService"), new Class<?>[]{type("ActivityManagerService"), type("PackageManagerService"), boolean.class}, null, h.pm, false);
            entries = (Map<Account, Object>) get(accounts, "mAccounts"); entries.clear();
            String code = (String) invoke(accounts, "packageIdentity", new Class<?>[]{String.class}, PACKAGE);
            for (Account account : List.of(ONE, TWO)) {
                Object entry = make(type("AccountManagerService$Entry"), new Class<?>[0]);
                set(entry, "owner", PACKAGE); set(entry, "ownerIdentity", code);
                set(entry, "password", "synthetic-password");
                ((Map<String, String>) get(entry, "tokens")).put("scope", "synthetic-token");
                entries.put(account, entry); h.identities.put(account, get(entry, "generation"));
            }
            invoke(accounts, "attachSyncManager", new Class<?>[]{type("SyncManager")}, h.sync);
            set(h.sync, "mAccounts", (Supplier<Account[]>) () -> {
                try { return (Account[]) invoke(accounts, "syncableAccounts", new Class<?>[0]); }
                catch (RuntimeException failure) { throw failure; }
                catch (Exception failure) { throw new AssertionError(failure); }
            });
            set(h.sync, "mAccountAccess", (BiPredicate<Account, String>) (account, pkg) -> {
                try {
                    if (accessChecks.incrementAndGet() == retireAtAccess) beginRemoval();
                    return access(account, pkg);
                } catch (RuntimeException failure) { throw failure; }
                catch (Exception failure) { throw new AssertionError(failure); }
            });
            ((Properties) get(h.sync, "mSettings")).clear(); h.setting("master", "true");
            h.setting("syncable." + TYPE + "/one/" + AUTHORITY, "1");
        }
        boolean access(Account account, String pkg) throws Exception {
            return (Boolean) invoke(accounts, "canAccessAccount", new Class<?>[]{Account.class, String.class}, account, pkg);
        }
        void beginRemoval() throws Exception { invoke(h.pm, "beginPackageRemoval", new Class<?>[]{type("ApkPackage")}, owner); }
        void endRemoval() throws Exception { invoke(h.pm, "endPackageRemoval", new Class<?>[]{type("ApkPackage")}, owner); }
        String periodic(Account account) throws Exception {
            Method id = type("SyncManager").getDeclaredMethod("id", Account.class, String.class); id.setAccessible(true);
            return "periodic." + id.invoke(null, account, AUTHORITY) + "|0";
        }
        @Override public void close() throws Exception {
            endRemoval(); h.close();
            ((ExecutorService) get(accounts, "mCalls")).shutdownNow();
            ((ExecutorService) get(accounts, "mSessionTimeout")).shutdownNow();
        }
    }

    @SuppressWarnings("unchecked")
    private static void removalAccessRaces(boolean wire) throws Exception {
        try (AccountAccessHarness a = new AccountAccessHarness(wire)) {
            Harness h = a.h;
            h.setting(a.periodic(TWO), "3600|"); invoke(h.sync, "save", new Class<?>[0]);
            java.nio.file.Path root = java.nio.file.Path.of(System.getenv("ANDROID_DATA"));
            byte[] settings = java.nio.file.Files.readAllBytes(root.resolve("system/sync.properties"));
            Object originalEntry = a.entries.get(ONE), generation = h.identities.get(ONE);
            h.enqueue(ONE, extras("captured-before-removal"));
            Object other = make(type("SyncManager$Pending"), new Class<?>[]{h.adapterType, Account.class, Bundle.class, long.class, boolean.class, Object.class},
                    h.adapter, TWO, extras("unrelated-delayed"), h.timer.now + 3600000, true, h.identities.get(TWO));
            ((List<Object>) get(h.sync, "mPending")).add(other);
            Map<String, Object> registry = new java.util.LinkedHashMap<>();
            registry.put(PACKAGE, make(type("Installer$Entry"), new Class<?>[]{String.class, String.class, long.class, long.class},
                    PACKAGE, new java.io.File((String) get(a.owner, "apkPath")).getParent(), 1L, 1L));
            Object installer = make(type("Installer"), new Class<?>[]{type("PackageManagerService"), type("ActivityTaskManagerService"), Map.class}, h.pm, h.atm, registry);
            invoke(installer, "writeRegistry", new Class<?>[0]);
            Object coordinator = get(h.pm, "mRemovalCoordinator");
            Class<?> coordinatorType = type("PackageManagerService$RemovalCoordinator");
            AtomicReference<SecurityException> dispatchFailure = new AtomicReference<>();
            set(h.pm, "mRemovalCoordinator", java.lang.reflect.Proxy.newProxyInstance(coordinatorType.getClassLoader(), new Class<?>[]{coordinatorType}, (proxy, method, args) -> {
                check((Boolean) invoke(h.pm, "isRemovalPending", new Class<?>[]{String.class}, PACKAGE), "Installer did not activate its removal barrier");
                try { a.access(ONE, PACKAGE); throw new AssertionError("Actual account access did not reject retirement"); }
                catch (SecurityException expected) { checks++; }
                try { h.timer.drain(); }
                catch (SecurityException failure) { dispatchFailure.set(failure); }
                // Even a broken lifecycle must reach the real rejected registry
                // commit, so the regression observes its state after rollback.
                return invoke(coordinator, "commit", new Class<?>[]{type("ApkPackage"), type("PackageRemovalStorage"), type("PackageManagerService$RemovalCommit")}, args);
            }));
            // No real app process is running when uninstall begins. Restore the
            // constructed process only after rollback, for the actual fresh bind.
            List<Object> processes = (List<Object>) get(h.atm, "mProcesses");
            List<Object> stopped = new ArrayList<>(processes); processes.clear();
            java.nio.file.Path blocked = root.resolve("system/packages.list.tmp"); java.nio.file.Files.createDirectory(blocked);
            try {
                try { invoke(installer, "uninstall", new Class<?>[]{String.class}, PACKAGE); throw new AssertionError("Registry failure was not reported"); }
                catch (java.io.IOException expected) { checks++; }
            } finally { java.nio.file.Files.delete(blocked); processes.addAll(stopped); }
            check(invoke(h.pm, "get", new Class<?>[]{String.class}, PACKAGE) == a.owner
                    && !(Boolean) invoke(h.pm, "isRemovalPending", new Class<?>[]{String.class}, PACKAGE), "Real Installer rollback did not restore the original package");
            check(a.entries.get(ONE) == originalEntry && get(originalEntry, "generation") == generation
                    && "synthetic-password".equals(get(originalEntry, "password"))
                    && "synthetic-token".equals(((Map<?, ?>) get(originalEntry, "tokens")).get("scope")), "Rollback changed the account's credentials or incarnation");
            check(java.util.Arrays.equals(settings, java.nio.file.Files.readAllBytes(root.resolve("system/sync.properties")))
                    && a.entries.containsKey(TWO), "Rollback changed persistent sync settings or another account");
            check(dispatchFailure.get() == null && h.active() == 0 && h.bindings() == 0 && h.starts.isEmpty(),
                    "Actual Installer rollback retained stranded sync work after access denial: " + dispatchFailure.get());
            check(h.pending() == 1 && ((List<?>) get(h.sync, "mPending")).contains(other), "Unrelated delayed request was lost during retirement");
            check(h.active() == 0 && (Long) invoke(h.sync, "nextDueElapsed", new Class<?>[0]) > h.timer.now,
                    "Canceled work retained active device-work ownership after rollback");
            h.enqueue(ONE, extras("fresh-after-rollback")); h.timer.drain();
            check(h.starts.size() == 1 && "fresh-after-rollback".equals(h.starts.get(0).extras.getString("tag")), "Fresh authorized sync remained blocked after rollback");
            h.finish(0, new SyncResult()); h.advance(3600000);
            check(h.starts.size() == 2 && h.starts.get(1).account.equals(TWO), "Unrelated delayed request did not resume after rollback");
            h.finish(1, new SyncResult());
        }
        try (AccountAccessHarness a = new AccountAccessHarness(wire)) {
            Harness h = a.h;
            // Admission and pre-bind checks succeed; revoke immediately before
            // the real account check on the already-bound connected callback.
            a.retireAtAccess = 3; h.enqueue(ONE, extras("connection-race")); h.timer.drain();
            check(a.accessChecks.get() == 3 && h.starts.isEmpty() && h.active() == 0 && h.bindings() == 0 && h.unbound == 1,
                    "Access rejection on connectedAdapter leaked a binding or reached the adapter");
            a.endRemoval(); h.enqueue(ONE, extras("fresh-after-connect-denial")); h.timer.drain();
            check(h.starts.size() == 1, "Fresh sync failed after connected-callback retirement");
            h.finish(0, new SyncResult());
        }
        try (AccountAccessHarness a = new AccountAccessHarness(wire)) {
            Harness h = a.h; a.retireAtAccess = 1;
            String periodic = a.periodic(ONE); h.setting(periodic, "3600|");
            synchronized (h.sync) { invoke(h.sync, "schedulePeriodic", new Class<?>[]{String.class, boolean.class}, periodic, true); }
            h.timer.drain();
            check(a.accessChecks.get() == 1 && h.starts.isEmpty() && h.pending() == 0 && h.active() == 0,
                    "Revoked periodic request escaped its callback or admitted unauthorized work");
            a.endRemoval(); h.timer.periodicDeliveries.get(0).run(); h.timer.drain();
            check(h.starts.size() == 1, "A later periodic tick could not sync after rollback of removal");
            h.finish(0, new SyncResult());
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
        for (String setting : new String[]{"master", "auto." + TYPE + "/one/" + AUTHORITY,
                "syncable." + TYPE + "/one/" + AUTHORITY}) {
            try (Harness h = new Harness(false, wire)) {
                h.enqueue(ONE, extras("retry-policy")); h.timer.drain(); h.finish(0, result("soft"));
                h.setting(setting, setting.startsWith("syncable.") ? "0" : "false");
                h.advance(60000);
                check(h.starts.size() == 1, "Due retry ignored changed sync policy: " + setting);
                check(h.active() == 0 && h.pending() == 0 && h.bindings() == 0,
                        "Rejected retry retained work or binding: " + setting);
            }
        }
        try (Harness h = new Harness(false, wire)) {
            h.duringCreate = () -> {
                try { h.setting("master", "false"); }
                catch (Exception failure) { throw new AssertionError(failure); }
            };
            h.enqueue(ONE, extras("policy-during-bind")); h.timer.drain();
            check(h.starts.isEmpty() && h.active() == 0 && h.bindings() == 0,
                    "Policy changed during binding still started adapter or leaked its connection");
        }
        try (Harness h = new Harness(false, wire)) {
            h.enqueue(ONE, extras("backoff")); h.timer.drain(); h.finish(0, result("soft"));
            h.setting("master", "false");
            Bundle manual = extras("manual"); manual.putBoolean(ContentResolver.SYNC_EXTRAS_MANUAL, true);
            h.request(manual); h.timer.drain();
            check(h.starts.size() == 2 && "manual".equals(h.starts.get(1).extras.getString("tag")),
                    "Manual request did not bypass existing backoff and disabled master sync");
            check(!manual.containsKey(ContentResolver.SYNC_EXTRAS_IGNORE_BACKOFF)
                    && !manual.containsKey(ContentResolver.SYNC_EXTRAS_IGNORE_SETTINGS),
                    "Manual normalization mutated caller extras");
            h.finish(1, result("soft"));
            check(h.starts.size() == 2, "Manual failure retried without backoff");
            h.advance(60000);
            check(h.starts.size() == 3 && "manual".equals(h.starts.get(2).extras.getString("tag")),
                    "Manual retry lost ignore-settings or resumed rejected automatic work");
        }
        try (Harness h = new Harness(false, wire)) {
            h.setting("syncable." + TYPE + "/one/" + AUTHORITY, "0");
            Bundle manual = extras("unsyncable"); manual.putBoolean(ContentResolver.SYNC_EXTRAS_MANUAL, true);
            h.request(manual); h.timer.drain();
            check(h.starts.isEmpty() && h.pending() == 0, "Manual request bypassed explicit unsyncable state");
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
                check(com.gesher.server.DeviceWork.dueWithin(0),
                        "Cancellation released device idle protection before the held callback retired");
            } finally { release.countDown(); worker.join(3000); }
            check(!worker.isAlive() && failure.get() == null, "Callback/cancellation did not finish: " + failure.get());
            h.timer.drain();
            check(h.active() == 0 && h.pending() == 0 && h.bindings() == 0, "Cancellation leaked late binding");
            check(!com.gesher.server.DeviceWork.dueWithin(0),
                    "Retired cancellation retained device idle protection");
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

    private static void retirement(boolean wire) throws Exception {
        try (Harness h = new Harness(false, wire)) {
            h.enqueue(ONE, extras("old")); h.timer.drain(); h.enqueue(ONE, extras("old-queued"));
            h.identities.put(ONE, new Object()); h.enqueue(ONE, extras("recreated")); h.timer.drain();
            check(h.cancelled == 1 && h.starts.size() == 2 && h.pending() == 0
                    && "recreated".equals(h.starts.get(1).extras.getString("tag")),
                    "A recreated account inherited an old pending request or stale active binding");
        }
        try (Harness h = new Harness(false, wire)) {
            Object retired = h.identities.get(ONE);
            h.enqueue(ONE, extras("retiring")); h.enqueue(TWO, extras("retained"));
            h.enqueue(ONE, extras("retiring-rerun")); h.timer.drain();
            Method id = type("SyncManager").getDeclaredMethod("id", Account.class, String.class); id.setAccessible(true);
            String periodic = "periodic." + id.invoke(null, ONE, AUTHORITY) + "|0";
            h.setting(periodic, "3600|");
            synchronized (h.sync) { invoke(h.sync, "schedulePeriodic", new Class<?>[]{String.class, boolean.class}, periodic, false); }
            h.retire("fixture.authenticator.owner", java.util.Set.of(ONE)); h.timer.drain();
            check(h.cancelled == 1 && h.unbound == 1 && h.starts.size() == 2 && h.starts.get(1).account.equals(TWO),
                    "Account retirement failed to cancel its binding and preserve unrelated queued work");
            check(h.pending() == 0 && ((Map<?, ?>) get(h.sync, "mPeriodic")).isEmpty(), "Retirement retained queued or periodic work");
            h.identities.put(ONE, new Object());
            SyncResult late = result("soft"); late.fullSyncRequested = true; late.delayUntil = h.timer.wall / 1000 + 120;
            h.finish(0, late);
            h.timer.periodicDeliveries.get(0).run(); h.timer.drain();
            invoke(h.sync, "enqueue", new Class<?>[]{h.adapterType, Account.class, Bundle.class, Object.class}, h.adapter, ONE, extras("stale-snapshot"), retired);
            h.timer.drain();
            check(h.pending() == 0 && h.starts.size() == 2 && ((Map<?, ?>) get(h.sync, "mBackoff")).isEmpty()
                    && ((Map<?, ?>) get(h.sync, "mDelayUntil")).isEmpty(), "Late result, old timer or captured request revived retired work");
            h.enqueue(ONE, extras("new-account")); h.finish(1, new SyncResult());
            check(h.starts.size() == 3 && "new-account".equals(h.starts.get(2).extras.getString("tag")), "Fresh account generation could not sync");
        }
        try (Harness h = new Harness(true, wire)) {
            h.enqueue(ONE, extras("one")); h.enqueue(TWO, extras("two")); h.timer.drain();
            SyncResult delayed = result("soft"); delayed.delayUntil = h.timer.wall / 1000 + 120;
            h.finish(0, delayed); h.finish(1, result("soft"));
            h.retire("fixture.authenticator.owner", java.util.Set.of(ONE)); h.timer.drain();
            check(h.pending() == 1 && ((Map<?, ?>) get(h.sync, "mBackoff")).size() == 1
                    && ((Map<?, ?>) get(h.sync, "mDelayUntil")).isEmpty(), "Retirement failed to isolate pending/backoff/delay cleanup");
        }
        try (Harness h = new Harness(true, wire)) {
            h.enqueue(ONE, extras("one")); h.enqueue(TWO, extras("two")); h.timer.drain();
            h.retire(PACKAGE, java.util.Set.of()); h.timer.drain();
            check(h.cancelled == 2 && h.unbound == 1 && h.bindings() == 0 && h.active() == 0 && h.pending() == 0,
                    "Removed adapter retained service bindings");
        }
    }

    public static void contracts(String group, boolean wire) throws Exception {
        if (group.equals("all") || group.equals("admission")) admission(wire);
        if (group.equals("all") || group.equals("clock")) clock(wire);
        if (group.equals("all") || group.equals("results")) results(wire);
        if (group.equals("all") || group.equals("cancellation")) cancellation(wire);
        if (group.equals("all") || group.equals("retirement")) retirement(wire);
        if (group.equals("all") || group.equals("removal-access")) removalAccessRaces(wire);
        if (wire && group.equals("all")) threaded();
    }
    public static void main(String[] args) throws Exception {
        contracts(args.length == 0 ? "all" : args[0], true);
        System.out.println("Sync manager contracts passed: " + checks + " checks; actual in-process AIDL/Parcel and service bindings");
    }
}
