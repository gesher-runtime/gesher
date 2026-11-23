/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.net.ConnectivityManager;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.IIntentReceiver;
import android.app.IApplicationThread;
import android.app.job.IJobService;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import com.gesher.net.IConnectivity;
import com.gesher.net.WindowsNetworks;
import com.gesher.server.ConnectivityService;
import com.gesher.server.WindowsHost;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;

/** Actual service/receiver/job owners, with only Windows network reads/waits controlled at JNI. */
public final class ConnectivityDemandHost {
    private static final String OFFLINE = "internet\t0\n";
    private static final String ONLINE = "internet\t1\nnet\t12\tethernet\t1500\t1000000\t1000000\t0\t0\t0\t1\t1\n";
    private static final String REPLACED = ONLINE.replace("net\t12\t", "net\t15\t");
    private static final Object NATIVE = new Object();
    private static final ArrayDeque<Boolean> events = new ArrayDeque<>();
    private static volatile boolean active;
    private static volatile String current = OFFLINE;
    private static boolean realTimeout;
    private static int reads, waits, waiting, maxWaiting, checks;

    public static String readSnapshot() {
        if (!active) return ConnectivityServiceSnapshotHost.readSnapshot();
        synchronized (NATIVE) { reads++; NATIVE.notifyAll(); return current; }
    }

    public static boolean waitForChange(int timeoutMs) throws InterruptedException {
        if (timeoutMs != 15_000) throw new AssertionError("NLM recovery interval changed: " + timeoutMs);
        synchronized (NATIVE) {
            waits++; waiting++; maxWaiting = Math.max(maxWaiting, waiting); NATIVE.notifyAll();
            try {
                long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs);
                while (events.isEmpty()) {
                    if (!realTimeout) NATIVE.wait();
                    else {
                        long left = deadline - System.nanoTime();
                        if (left <= 0) return false;
                        TimeUnit.NANOSECONDS.timedWait(NATIVE, left);
                    }
                }
                return events.removeFirst();
            } finally { waiting--; NATIVE.notifyAll(); }
        }
    }

    private static void signal(boolean changed) {
        synchronized (NATIVE) { events.addLast(changed); NATIVE.notifyAll(); }
    }
    private static synchronized void check(boolean value, String why) {
        if (!value) throw new AssertionError(why);
        checks++;
    }
    private static void await(BooleanSupplier done, String why) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!done.getAsBoolean() && System.nanoTime() < deadline) Thread.sleep(2);
        check(done.getAsBoolean(), why);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static Object get(Object owner, String name) throws Exception {
        return field(owner.getClass(), name).get(owner);
    }
    private static Method method(Class<?> type, String name, Class<?>... signature) throws Exception {
        Method result = type.getDeclaredMethod(name, signature); result.setAccessible(true); return result;
    }
    private static Object call(Object owner, String name, Class<?>[] signature, Object... args) throws Exception {
        try { return method(owner.getClass(), name, signature).invoke(owner, args); }
        catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception e) throw e;
            if (failure.getCause() instanceof Error e) throw e;
            throw failure;
        }
    }
    private static Class<?> server(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static ConnectivityService service() throws Exception {
        return service(null, null);
    }
    private static ConnectivityService service(Object pm, Object atm) throws Exception {
        Constructor<?> make = ConnectivityService.class.getDeclaredConstructor(
                server("PackageManagerService"), server("ActivityTaskManagerService"));
        make.setAccessible(true);
        return (ConnectivityService) make.newInstance(pm, atm);
    }
    private static Object make(Class<?> type, Class<?>[] signature, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(signature); constructor.setAccessible(true);
        return constructor.newInstance(values);
    }
    private static Thread watcher(ConnectivityService service) throws Exception {
        synchronized (service) { return (Thread) get(service, "mWatcher"); }
    }
    private static int readCount() { synchronized (NATIVE) { return reads; } }
    private static int waitCount() { synchronized (NATIVE) { return waits; } }
    private static int waitingCount() { synchronized (NATIVE) { return waiting; } }
    private static void drained(ConnectivityService service) throws Exception {
        Thread retiring = watcher(service);
        if (retiring != null) { signal(false); retiring.join(5_000); }
        check(watcher(service) == null, "Last owner left a live periodic connectivity watcher");
        check(waitingCount() == 0, "Native wait remained after the watcher retired");
        check(!((List<?>)field(WindowsHost.class, "sListeners").get(null)).contains(service),
                "Retired service retained its resume subscription");
    }

    private static final class Endpoint implements IConnectivity.IListener {
        final Set<IBinder.DeathRecipient> deaths = Collections.newSetFromMap(new IdentityHashMap<>());
        final List<String> observed = Collections.synchronizedList(new ArrayList<>());
        final IBinder binder;
        boolean failLink, dieInLink;
        volatile Runnable callback;
        Endpoint() {
            binder = (IBinder) Proxy.newProxyInstance(IBinder.class.getClassLoader(), new Class<?>[]{IBinder.class},
                    (proxy, member, args) -> {
                        switch (member.getName()) {
                            case "linkToDeath":
                                if (failLink) throw new RemoteException("already dead");
                                IBinder.DeathRecipient death = (IBinder.DeathRecipient) args[0];
                                synchronized (deaths) { deaths.add(death); }
                                if (dieInLink) die();
                                return null;
                            case "unlinkToDeath": synchronized (deaths) { return deaths.remove(args[0]); }
                            case "isBinderAlive": case "pingBinder": return true;
                            case "equals": return proxy == args[0];
                            case "hashCode": return System.identityHashCode(proxy);
                            case "toString": return "ConnectivityDemandEndpoint";
                            default: return null;
                        }
                    });
        }
        @Override public IBinder asBinder() { return binder; }
        @Override public void onSnapshot(String snapshot) {
            observed.add(snapshot);
            Runnable next = callback;
            if (next != null) next.run();
        }
        void die() {
            List<IBinder.DeathRecipient> linked;
            synchronized (deaths) { linked = new ArrayList<>(deaths); deaths.clear(); }
            for (IBinder.DeathRecipient death : linked) death.binderDied();
        }
        int links() { synchronized (deaths) { return deaths.size(); } }
    }

    private static void resume(ConnectivityService service) {
        ((WindowsHost.Listener)(Object) service).onWindowsEvents(WindowsHost.EVENT_RESUMED);
    }

    private static void lifecycle() throws Exception {
        ConnectivityService service = service();
        check(service.getSnapshot().equals(OFFLINE), "Unwatched initial query changed state");
        current = ONLINE;
        check(service.getSnapshot().equals(ONLINE) && watcher(service) == null,
                "Query-only reconnect did not refresh without creating an observer");
        Endpoint first = new Endpoint();
        service.addListener(first);
        await(() -> waitingCount() == 1, "First client did not reach the native wait");
        Thread original = watcher(service);
        service.addListener(first);
        check(first.links() == 1, "Duplicate subscription retained an extra death link");
        int readsBefore = readCount(), waitsBefore = waitCount();
        service.removeListener(first);
        check(first.links() == 0, "Explicit removal retained its remote death recipient");
        current = REPLACED;
        check(service.getSnapshot().equals(REPLACED), "Draining watcher served stale state to a query");
        Endpoint replacement = new Endpoint();
        service.addListener(replacement);
        check(watcher(service) == original, "Reacquisition created a competing native waiter");
        check(service.getSnapshot().equals(REPLACED), "Reacquisition reused the old cache");
        signal(true);
        await(() -> replacement.observed.contains(REPLACED), "Replacement missed the current network");
        await(() -> waitCount() > waitsBefore, "Observer did not resume its native wait");
        service.removeListener(replacement);
        int releasedReads = readCount();
        drained(service);
        check(readCount() == releasedReads, "Final native return enumerated networks after ownership ended");
        check(readCount() > readsBefore, "Fixture never exercised an active snapshot read");
        int quietReads = readCount(), quietWaits = waitCount();
        current = OFFLINE;
        resume(service);
        Thread.sleep(30);
        check(readCount() == quietReads && waitCount() == quietWaits,
                "Idle resume restarted unnecessary snapshots or waits");
        Endpoint later = new Endpoint(); service.addListener(later);
        check(service.getSnapshot().equals(OFFLINE), "New watcher missed a network change while idle");
        await(() -> waitingCount() == 1, "New watcher did not start after completed retirement");
        later.die(); drained(service);
        check(later.links() == 0, "Endpoint death retained a subscription");

        Endpoint failed = new Endpoint(); failed.failLink = true;
        try { service.addListener(failed); throw new AssertionError("Dead endpoint was admitted"); }
        catch (RemoteException expected) { checks++; }
        check(watcher(service) == null && failed.links() == 0, "Failed death-link acquired observation");
        Endpoint dying = new Endpoint(); dying.dieInLink = true;
        service.addListener(dying);
        check(watcher(service) == null && dying.links() == 0, "Death during attachment leaked observation");

        Endpoint churn = new Endpoint(); service.addListener(churn);
        await(() -> waitingCount() == 1, "Churn watcher did not enter its wait");
        original = watcher(service);
        IBinder.DeathRecipient oldDeath;
        synchronized (churn.deaths) { oldDeath = churn.deaths.iterator().next(); }
        service.removeListener(churn); service.addListener(churn); oldDeath.binderDied();
        check(churn.links() == 1 && watcher(service) == original,
                "Late death of an old registration removed its replacement");
        for (int i = 0; i < 10_000; i++) {
            service.removeListener(churn); service.addListener(churn);
            if (watcher(service) != original || churn.links() != 1) {
                throw new AssertionError("Observer/death-link growth in cycle " + i);
            }
        }
        checks++;
        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> raced = new AtomicReference<>();
        List<Thread> racers = new ArrayList<>();
        for (int i = 0; i < 8; i++) {
            Thread thread = new Thread(() -> {
                try {
                    start.await();
                    for (int j = 0; j < 250; j++) { service.addListener(churn); service.removeListener(churn); }
                } catch (Throwable failure) { raced.set(failure); }
            });
            racers.add(thread); thread.start();
        }
        start.countDown();
        for (Thread thread : racers) thread.join(5_000);
        check(raced.get() == null && racers.stream().noneMatch(Thread::isAlive),
                "Concurrent registration/removal failed or deadlocked: " + raced.get());
        service.removeListener(churn); drained(service);
        check(churn.links() == 0 && maxWaiting == 1, "Churn grew native waiters or death recipients");
    }

    private static void resumeAndCallbacks() throws Exception {
        current = ONLINE;
        ConnectivityService service = service();
        Endpoint listener = new Endpoint();
        AtomicReference<Throwable> failed = new AtomicReference<>();
        AtomicInteger concurrent = new AtomicInteger(), maximum = new AtomicInteger();
        listener.callback = () -> {
            int count = concurrent.incrementAndGet(); maximum.accumulateAndGet(count, Math::max);
            try { if (Thread.holdsLock(service)) throw new AssertionError("Application callback ran under service monitor"); }
            catch (Throwable failure) { failed.set(failure); }
            finally { concurrent.decrementAndGet(); }
        };
        service.addListener(listener);
        await(() -> waitingCount() == 1, "Resume fixture watcher did not wait");
        current = ONLINE.replace("\t0\t0\t0\t1\t1", "\t1\t0\t0\t1\t1");
        String metered = current;
        signal(false);
        await(() -> listener.observed.contains(metered), "Demanded NLM cost fallback was disabled");
        await(() -> waitingCount() == 1, "Fallback did not restore its bounded native wait");
        current = REPLACED;
        int before = waitCount();
        resume(service);
        check(listener.observed.contains(REPLACED) && service.getSnapshot().equals(REPLACED),
                "Resume did not resynchronize without a native interface event");
        check(waitCount() == before, "Resume started another native wait");

        CountDownLatch entered = new CountDownLatch(1), released = new CountDownLatch(1);
        listener.callback = () -> {
            int count = concurrent.incrementAndGet(); maximum.accumulateAndGet(count, Math::max);
            try { entered.countDown(); if (!released.await(5, TimeUnit.SECONDS)) throw new AssertionError("Callback release timed out"); }
            catch (Throwable failure) { failed.set(failure); }
            finally { concurrent.decrementAndGet(); }
        };
        current = OFFLINE;
        Thread update = new Thread(() -> resume(service)); update.start();
        check(entered.await(5, TimeUnit.SECONDS), "Slow callback did not begin");
        current = ONLINE;
        for (int i = 0; i < 100; i++) resume(service);
        check(concurrent.get() == 1, "Concurrent refresh entered another callback");
        listener.callback = null;
        released.countDown(); update.join(5_000);
        check(!update.isAlive() && maximum.get() == 1 && failed.get() == null,
                "Refresh serialization failed or held a callback lock: " + failed.get());
        check(listener.observed.get(listener.observed.size() - 1).equals(ONLINE),
                "Coalesced resume did not publish the newest actual state");

        Endpoint reentrant = new Endpoint();
        CountDownLatch done = new CountDownLatch(1);
        reentrant.callback = () -> {
            try {
                Thread remover = new Thread(() -> service.removeListener(reentrant)); remover.start(); remover.join(3_000);
                if (remover.isAlive()) throw new AssertionError("Callback removal deadlocked on an observer lock");
                done.countDown();
            } catch (Throwable failure) { failed.set(failure); }
        };
        service.addListener(reentrant);
        current = REPLACED; resume(service);
        check(done.await(5, TimeUnit.SECONDS) && failed.get() == null, "Reentrant removal failed: " + failed.get());
        check(reentrant.links() == 0, "Reentrant removal retained its death recipient");
        listener.callback = () -> { throw new IllegalStateException("fixture failed callback"); };
        current = OFFLINE; resume(service);
        drained(service);
        check(listener.links() == 0, "Failed callback retained observer ownership");
    }

    private static void stickyQueries() throws Exception {
        ConnectivityService service = service();
        Class<?> sticky = server("StickyBroadcasts");
        Method query = method(sticky, "match", IntentFilter.class);
        IntentFilter filter = new IntentFilter(ConnectivityManager.CONNECTIVITY_ACTION);
        current = OFFLINE;
        Intent offline = (Intent) query.invoke(null, filter);
        check(offline.getBooleanExtra(ConnectivityManager.EXTRA_NO_CONNECTIVITY, false), "Offline sticky query was not current");
        current = ONLINE;
        Intent online = (Intent) query.invoke(null, filter);
        check(!online.getBooleanExtra(ConnectivityManager.EXTRA_NO_CONNECTIVITY, false), "Sticky-only query kept an old network");
        check(watcher(service) == null, "Sticky-only query permanently acquired connectivity observation");
        check(query.invoke(null, new IntentFilter("gesher.unrelated")) == null,
                "Unrelated receiver query got a connectivity sticky");
    }

    private static void registeredReceivers() throws Exception {
        // Reuse installed package/process metadata, without replacing any broadcast or
        // connectivity operation. The receiver calls the real register/unregister and
        // process-death entry points, and publication traverses AMS/BroadcastController.
        Class<?> harnessType = Class.forName("gesher.services.JobSchedulerConstraintContractTest$Harness");
        Object harness = make(harnessType, new Class<?>[0]);
        Object previousServices = field(server("Services"), "sLast").get(null);
        Object pm = get(harness, "pm"), atm = get(harness, "atm"), am = get(harness, "am");
        ConnectivityService service = service(pm, atm);
        Object broadcasts = make(server("BroadcastController"),
                new Class<?>[]{server("ActivityTaskManagerService"), server("PackageManagerService")}, atm, pm);
        field(am.getClass(), "mBroadcasts").set(am, broadcasts);
        Object created = make(server("Services$Created"), new Class<?>[]{server("PackageManagerService"),
                server("ActivityTaskManagerService"), server("ActivityManagerService"), server("ClipboardService"), java.util.Map.class},
                pm, atm, am, null, java.util.Map.of());
        field(server("Services"), "sLast").set(null, created);
        Object process = ((List<?>)get(atm, "mProcesses")).get(0);
        List<Intent> delivered = Collections.synchronizedList(new ArrayList<>());
        AtomicReference<Throwable> failure = new AtomicReference<>();
        IApplicationThread app = new IApplicationThread.Default() {
            final Binder binder = new Binder();
            @Override public IBinder asBinder() { return binder; }
            @Override public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent,
                    int resultCode, String data, Bundle extras, boolean ordered, boolean sticky,
                    boolean assumeDelivered, int user, int state, int uid, String pkg) {
                if (Thread.holdsLock(service) || Thread.holdsLock(broadcasts)) {
                    failure.set(new AssertionError("Broadcast callback ran under an owner monitor"));
                }
                delivered.add(new Intent(intent));
            }
        };
        field(process.getClass(), "thread").set(process, app);
        String pkg = (String)get(process, "packageName");
        int pid = (Integer)get(process, "pid");
        Class<?>[] registration = {IApplicationThread.class, String.class, IIntentReceiver.class,
                IntentFilter.class, int.class, String.class, int.class};
        IntentFilter filter = new IntentFilter(ConnectivityManager.CONNECTIVITY_ACTION);
        IIntentReceiver first = new IIntentReceiver.Stub() {
            @Override public void performReceive(Intent i, int c, String d, Bundle b, boolean o, boolean s, int u) { }
        };
        IIntentReceiver second = new IIntentReceiver.Stub() {
            @Override public void performReceive(Intent i, int c, String d, Bundle b, boolean o, boolean s, int u) { }
        };
        try {
            current = OFFLINE;
            call(broadcasts, "registerReceiver", registration, app, pkg, null, filter, pid, null,
                    android.content.Context.RECEIVER_EXPORTED);
            check(watcher(service) == null, "Actual receiver=null query acquired a watcher");
            call(broadcasts, "registerReceiver", registration, app, pkg, first, filter, pid, null,
                    android.content.Context.RECEIVER_EXPORTED);
            await(() -> waitingCount() == 1, "Registered connectivity receiver did not own observation");
            check(delivered.size() == 1 && delivered.get(0).getBooleanExtra(ConnectivityManager.EXTRA_NO_CONNECTIVITY, false),
                    "Registered receiver did not get the current sticky state");
            current = ONLINE;
            signal(true);
            await(() -> delivered.size() == 2, "Active receiver missed the normal connectivity broadcast");
            check(!delivered.get(1).getBooleanExtra(ConnectivityManager.EXTRA_NO_CONNECTIVITY, false),
                    "Network reconnect broadcast reported stale state");
            call(broadcasts, "registerReceiver", registration, app, pkg, first, filter, pid, null,
                    android.content.Context.RECEIVER_EXPORTED);
            call(broadcasts, "registerReceiver", registration, app, pkg, second, filter, pid, null,
                    android.content.Context.RECEIVER_EXPORTED);
            call(broadcasts, "unregisterReceiver", new Class<?>[]{IIntentReceiver.class}, first);
            check(watcher(service) != null, "One unregister dropped another receiver's observation");
            call(broadcasts, "processDied", new Class<?>[]{server("ActivityTaskManagerService$ProcessRecord")}, process);
            drained(service);
            check(((java.util.Map<?, ?>)get(broadcasts, "mRegistered")).isEmpty(), "Process death retained receiver generations");
            check(failure.get() == null, "Receiver delivery held an owner lock: " + failure.get());

            IntentFilter unmatched = new IntentFilter(ConnectivityManager.CONNECTIVITY_ACTION);
            unmatched.addDataScheme("https");
            call(broadcasts, "registerReceiver", registration, app, pkg, first, unmatched, pid, null,
                    android.content.Context.RECEIVER_EXPORTED);
            check(watcher(service) == null, "A filter that cannot match connectivity acquired observation");
            call(broadcasts, "unregisterReceiver", new Class<?>[]{IIntentReceiver.class}, first);
        } finally {
            call(broadcasts, "processDied", new Class<?>[]{server("ActivityTaskManagerService$ProcessRecord")}, process);
            ((AutoCloseable)harness).close();
            ((java.util.concurrent.ScheduledThreadPoolExecutor)get(broadcasts, "mTimeouts")).shutdownNow();
            field(server("Services"), "sLast").set(null, previousServices);
            if (watcher(service) != null) drained(service);
        }
    }

    private static void runningJobSignals() throws Exception {
        current = ONLINE;
        Class<?> harnessType = Class.forName("gesher.services.JobSchedulerConstraintContractTest$Harness");
        Object harness = make(harnessType, new Class<?>[]{boolean.class, boolean.class, boolean.class},
                true, true, true);
        Object scheduler = get(harness, "scheduler");
        ConnectivityService connectivity = (ConnectivityService)get(harness, "connectivity");
        AtomicInteger starts = new AtomicInteger(), stops = new AtomicInteger(), changes = new AtomicInteger();
        AtomicReference<JobParameters> started = new AtomicReference<>(), stopped = new AtomicReference<>(), changed = new AtomicReference<>();
        IJobService jobService = new IJobService.Stub() {
            @Override public void startJob(JobParameters params) throws RemoteException {
                started.set(params); starts.incrementAndGet();
                params.getCallback().acknowledgeStartMessage(params.getJobId(), true);
            }
            @Override public void stopJob(JobParameters params) throws RemoteException {
                stopped.set(params); stops.incrementAndGet();
                params.getCallback().acknowledgeStopMessage(params.getJobId(), false);
            }
            @Override public void onNetworkChanged(JobParameters params) { changed.set(params); changes.incrementAndGet(); }
            @Override public void getTransferredDownloadBytes(JobParameters params, JobWorkItem item) { }
            @Override public void getTransferredUploadBytes(JobParameters params, JobWorkItem item) { }
        };
        Thread worker = null;
        try {
            JobInfo info = new JobInfo.Builder(301, new ComponentName("gesher.jobscheduler.test",
                    "gesher.jobscheduler.test.TestJobService")).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build();
            call(harness, "schedule", new Class<?>[]{JobInfo.class}, info);
            Object run = call(harness, "begin", new Class<?>[]{int.class}, 301);
            call(run, "started", new Class<?>[]{IBinder.class}, jobService.asBinder());
            check(starts.get() == 1 && started.get().getNetwork().equals(new android.net.Network(112)),
                    "Running job did not receive its real observed satisfying network");
            // Binding is local, but the actual scheduler worker consumes network signals
            // and performs handover/stop dispatch. No constraint override is installed.
            field(scheduler.getClass(), "mThread").set(scheduler, null);
            call(scheduler, "start", new Class<?>[0]);
            worker = (Thread)get(scheduler, "mThread");
            await(() -> waitingCount() == 1, "Running job did not retain the native observer");
            current = REPLACED; signal(true);
            await(() -> changes.get() != 0 && changed.get().getNetwork().equals(new android.net.Network(115)),
                    "Native network event did not reach running JobService.onNetworkChanged");
            current = OFFLINE;
            // Exercise the actual WindowsHost order where JobScheduler sees resume first.
            ((WindowsHost.Listener)scheduler).onWindowsEvents(WindowsHost.EVENT_RESUMED);
            resume(connectivity);
            await(() -> stops.get() == 1, "Resume without interface events did not stop a network-constrained job");
            check(stopped.get().getStopReason() == JobParameters.STOP_REASON_CONSTRAINT_CONNECTIVITY,
                    "Resume used a stale satisfying network or changed the stop contract");
            await(() -> {
                synchronized (connectivity) {
                    try { return ((List<?>)get(connectivity, "mSystemListeners")).isEmpty(); }
                    catch (Exception e) { throw new RuntimeException(e); }
                }
            }, "Acknowledged stop retained mandatory network ownership");
            drained(connectivity);
        } finally {
            ((AutoCloseable)harness).close();
            if (worker != null) { worker.interrupt(); worker.join(3_000); }
            WindowsHost.removeListener((WindowsHost.Listener)scheduler);
            if (watcher(connectivity) != null) drained(connectivity);
        }
    }

    private static void trace(long durationMs) throws Exception {
        realTimeout = true;
        ConnectivityService service = service(); Endpoint listener = new Endpoint();
        service.addListener(listener);
        await(() -> waitingCount() == 1, "Trace observer did not reach native wait");
        service.removeListener(listener);
        // Drain the one wait already in flight, then stabilize the two compared sources.
        signal(false); Thread.sleep(250);
        int initialReads = readCount(), initialWaits = waitCount();
        long start = System.nanoTime();
        Thread.sleep(durationMs);
        long elapsed = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - start);
        int extraReads = readCount() - initialReads, extraWaits = waitCount() - initialWaits;
        System.out.println("ConnectivityDemandHost idle_trace elapsed_ms=" + elapsed + " snapshots="
                + extraReads + " waits=" + extraWaits + " retained_watcher=" + (watcher(service) != null));
        check(extraReads == 0 && extraWaits == 0 && watcher(service) == null,
                "No-owner trace retained periodic connectivity work");
    }

    public static void contracts(String selected, long traceMillis) throws Exception {
        active = true;
        field(WindowsNetworks.class, "sLoaded").set(null, true);
        field(ConnectivityService.class, "sInSystemServer").set(null, true);
        field(WindowsHost.class, "sAvailable").set(null, false);
        if (selected.equals("idle-trace")) trace(traceMillis);
        else {
            lifecycle();
            resumeAndCallbacks();
            stickyQueries();
            registeredReceivers();
            gesher.services.JobSchedulerConstraintContractTest.connectivityOwnershipContracts(false);
            if (waitingCount() != 0) signal(false);
            await(() -> waitingCount() == 0, "Job completion retained a native connectivity waiter");
            runningJobSignals();
        }
        active = false;
        System.out.println("ConnectivityDemandHost all passed: " + checks + " checks; max_native_waiters=" + maxWaiting);
        System.out.println("Production Java ownership/callback paths; controlled native reads/waits. No Windows notification/ART/whole-process CPU claim.");
    }
}
