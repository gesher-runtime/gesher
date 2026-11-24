/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.app.AppOpsManager;
import android.os.IBinder;
import com.gesher.server.ApkPackage;
import com.gesher.server.AppOpsService;
import gesher.services.AppOpsContractTest;
import java.lang.management.ManagementFactory;
import java.lang.management.ThreadInfo;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises the actual fixture monitor probe against a real active AppOps ledger. */
public final class AppOpsMonitorProbeHost {
    private static int checks;
    private static Object get(Object owner, String name) throws Exception {
        Field field = owner.getClass().getDeclaredField(name); field.setAccessible(true); return field.get(owner);
    }
    private static Object make(String name) throws Exception {
        Constructor<?> constructor = Class.forName(name).getDeclaredConstructor();
        constructor.setAccessible(true); return constructor.newInstance();
    }
    private static void invoke(Method method, Object receiver, Object... arguments) throws Exception {
        try { method.invoke(receiver, arguments); }
        catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception e) throw e;
            if (failure.getCause() instanceof Error e) throw e;
            throw failure;
        }
    }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message); checks++;
    }
    private static void await(CountDownLatch latch) throws Exception {
        check(latch.await(5, TimeUnit.SECONDS), "Controlled real-monitor owner did not arrive");
    }
    private static Thread worker(String name, AtomicReference<Throwable> error, Runnable task) {
        Thread thread = new Thread(() -> {
            try { task.run(); } catch (Throwable failure) { error.compareAndSet(null, failure); }
        }, name);
        thread.setDaemon(true); thread.start(); return thread;
    }
    private static void blockedOn(Thread thread, Object monitor) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (thread.isAlive() && System.nanoTime() < deadline) {
            ThreadInfo info = ManagementFactory.getThreadMXBean().getThreadInfo(thread.threadId());
            if (info != null && info.getThreadState() == Thread.State.BLOCKED && info.getLockInfo() != null
                    && info.getLockInfo().getIdentityHashCode() == System.identityHashCode(monitor)) {
                checks++; return;
            }
            Thread.sleep(1);
        }
        throw new AssertionError("The real fixture probe did not inspect its required monitor");
    }
    private static void join(Thread thread, AtomicReference<Throwable> error) throws Exception {
        thread.join(3000);
        if (thread.isAlive()) {
            long[] deadlocked = ManagementFactory.getThreadMXBean().findDeadlockedThreads();
            if (deadlocked != null) for (ThreadInfo info : ManagementFactory.getThreadMXBean()
                    .getThreadInfo(deadlocked, true, true)) System.err.println(info);
            throw new AssertionError("Fixture monitor probe deadlocked with actual AppOps refresh");
        }
        checks++;
        if (error.get() != null) throw new AssertionError("Real monitor/refresh operation failed", error.get());
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Class<?> locales = Class.forName("com.gesher.app.locale.SystemLocale");
        Field hour = locales.getDeclaredField("sIs24Hour"); hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        Field list = locales.getDeclaredField("sLocales"); list.setAccessible(true);
        list.set(null, new android.os.LocaleList(java.util.Locale.ROOT));
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        Object harness = make("gesher.services.AppOpsContractTest$Harness");
        Object pm = get(harness, "pm"), policy = pm.getClass().getMethod("permissions").invoke(pm);
        AppOpsService service = (AppOpsService)get(harness, "service");
        Object operationLock = get(service, "mLock");
        ApkPackage owner = (ApkPackage)get(harness, "owner");
        IBinder token = (IBinder)make("gesher.services.AppOpsContractTest$Token");
        Method probe = AppOpsContractTest.class.getDeclaredMethod("inspectRetirementMonitors",
                harness.getClass(), Object.class, Object.class); probe.setAccessible(true);
        Method refresh = AppOpsService.class.getDeclaredMethod("refreshRunningOperations"); refresh.setAccessible(true);
        ExecutorService events = (ExecutorService)get(service, "mEvents");
        check(service.startOperation(token, AppOpsManager.OP_VIBRATE, owner.uid(), owner.packageName, null,
                false, false, null, false, 0, -1).getOpMode() == AppOpsManager.MODE_ALLOWED,
                "The actual permission-checked attributed operation must start");
        events.submit(() -> { }).get(5, TimeUnit.SECONDS);
        events.submit(() -> { }).get(5, TimeUnit.SECONDS);
        AtomicReference<Throwable> error = new AtomicReference<>();
        Runnable inspect = () -> {
            try { invoke(probe, null, harness, policy, operationLock); }
            catch (Exception failure) { throw new AssertionError(failure); }
        };
        // All three original availability checks remain required. Omitting any
        // monitor lets the probe finish while that exact monitor is held.
        for (Object monitor : List.of(pm, policy, operationLock)) {
            CountDownLatch held = new CountDownLatch(1), release = new CountDownLatch(1);
            Thread holder = worker("appops-fixture-held-monitor", error, () -> {
                synchronized (monitor) {
                    held.countDown();
                    try { if (!release.await(5, TimeUnit.SECONDS)) throw new AssertionError("Monitor holder timed out"); }
                    catch (InterruptedException failure) { throw new AssertionError(failure); }
                }
            });
            await(held);
            Thread inspector = worker("appops-fixture-probe", error, inspect);
            try { blockedOn(inspector, monitor); } finally { release.countDown(); }
            join(holder, error); join(inspector, error);
        }
        CountDownLatch held = new CountDownLatch(1), refreshNow = new CountDownLatch(1);
        Thread refresher = worker("appops-actual-running-refresh", error, () -> {
            synchronized (operationLock) {
                held.countDown();
                try {
                    if (!refreshNow.await(5, TimeUnit.SECONDS)) throw new AssertionError("Actual refresh was not released");
                    invoke(refresh, service);
                } catch (Exception failure) { throw new AssertionError(failure); }
            }
        });
        await(held);
        Thread inspector = worker("appops-fixture-probe", error, inspect);
        try { blockedOn(inspector, operationLock); } finally { refreshNow.countDown(); }
        // The old nested probe owns PermissionPolicy here, while the real refresh
        // owns mLock and reads that policy. Sequential availability checks cannot
        // introduce this test-only cycle. A failure prints the actual owner stacks.
        join(refresher, error); join(inspector, error);
        check(service.isOperationActive(AppOpsManager.OP_VIBRATE, owner.uid(), owner.packageName),
                "Monitor inspection preserves the real active client");
        service.finishOperation(token, AppOpsManager.OP_VIBRATE, owner.uid(), owner.packageName, null);
        check(((List<?>)get(token, "recipients")).isEmpty(), "Actual completion releases client death ownership");
        for (ExecutorService executor : List.of(events, (ExecutorService)get(pm, "mPackageEventExecutor"),
                (ExecutorService)get(get(harness, "atm"), "mTaskCallbacks"))) {
            executor.shutdown(); check(executor.awaitTermination(5, TimeUnit.SECONDS), "Owned fixture executor retires");
        }
        System.out.println("AppOpsMonitorProbeHost all passed: " + checks + " checks; actual service refresh and all three fixture monitors");
    }
}
