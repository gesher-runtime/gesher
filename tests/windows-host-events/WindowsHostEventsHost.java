/* Complete production WindowsHost; the native fixture identifies its OS boundaries. */
package com.gesher.server;

import java.lang.management.ManagementFactory;
import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

public final class WindowsHostEventsHost {
    private static native void emit(int events);
    private static native int[] stats();
    private static int checks;
    private static void check(boolean value, String name) {
        if (!value) throw new AssertionError(name);
        checks++;
    }
    private static void until(BooleanSupplier condition, String name) throws Exception {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(8);
        while (!condition.getAsBoolean() && System.nanoTime() < end) Thread.sleep(2);
        check(condition.getAsBoolean(), name);
    }
    private static Thread dispatcher() {
        var threads = Thread.getAllStackTraces().keySet().stream()
                .filter(t -> t.getName().equals("windows-events") && t.isAlive()).toList();
        check(threads.size() <= 1, "at most one server dispatcher");
        return threads.isEmpty() ? null : threads.get(0);
    }
    private static void unavailable() {
        check(!WindowsHost.available(), "absent native implementation keeps fallback explicit");
        WindowsHost.Listener listener = events -> { throw new AssertionError("fabricated event"); };
        WindowsHost.addListener(listener); WindowsHost.removeListener(listener);
        check(dispatcher() == null, "unavailable native backend starts no dispatcher");
        check(WindowsHost.waitActivation(20) == null, "unavailable toast wait returns without native dispatch");
        check(WindowsHost.alarmWait(System.currentTimeMillis() + 60_000, false, 0) == WindowsHost.ALARM_TIMEOUT,
                "finite fallback alarm timeout remains independent");
        WindowsHost.alarmKick();
        check(WindowsHost.alarmWait(0, false, 1000) == WindowsHost.ALARM_KICKED,
                "fallback alarm kick remains independent");
    }
    private static void deadline() throws Exception {
        Thread noise = new Thread(() -> {
            long end = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(300);
            while (System.nanoTime() < end) {
                emit(WindowsHost.EVENT_SETTING_CHANGED);
                try { Thread.sleep(5); } catch (InterruptedException done) { return; }
            }
        });
        noise.start();
        long start = System.nanoTime();
        check(WindowsHost.waitActivation(120) == null, "non-toast events do not invent an activation");
        long elapsed = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - start);
        noise.interrupt(); noise.join();
        check(elapsed >= 90 && elapsed < 2000, "toast timeout retains one finite deadline amid unrelated events");
        check(stats()[4] > 0 && stats()[0] == 0 && dispatcher() == null,
                "foreground finite native waits do not start the server dispatcher");
    }
    private static void dispatch(int idleSeconds) throws Exception {
        var listeners = new ArrayList<WindowsHost.Listener>();
        var starters = new ArrayList<Thread>();
        CountDownLatch start = new CountDownLatch(1);
        for (int i = 0; i < 12; i++) {
            WindowsHost.Listener listener = events -> {};
            listeners.add(listener);
            starters.add(new Thread(() -> {
                try { start.await(); } catch (InterruptedException failure) { throw new AssertionError(failure); }
                WindowsHost.addListener(listener);
            }));
        }
        starters.forEach(Thread::start); start.countDown();
        for (Thread thread : starters) thread.join(8000);
        check(starters.stream().noneMatch(Thread::isAlive), "concurrent registration completes");
        check(stats()[0] == 1 && SystemServer.window, "concurrent listeners start one actual native event window");
        until(() -> stats()[1] > 0, "dispatcher enters native event wait");
        Thread owner = dispatcher(); check(owner != null && owner.isDaemon(), "server dispatcher remains a daemon");
        listeners.forEach(WindowsHost::removeListener);

        AtomicInteger reentered = new AtomicInteger(), late = new AtomicInteger(), delivered = new AtomicInteger();
        WindowsHost.Listener added = events -> {
            if ((events & WindowsHost.EVENT_TIME_CHANGED) != 0) late.incrementAndGet();
        };
        WindowsHost.Listener[] self = new WindowsHost.Listener[1];
        self[0] = events -> {
            if ((events & WindowsHost.EVENT_TIME_CHANGED) == 0) return;
            WindowsHost.removeListener(self[0]); WindowsHost.addListener(added);
            reentered.incrementAndGet(); throw new IllegalStateException("deliberate listener failure");
        };
        WindowsHost.Listener other = events -> {
            if ((events & WindowsHost.EVENT_TIME_CHANGED) != 0) delivered.incrementAndGet();
        };
        WindowsHost.addListener(self[0]); WindowsHost.addListener(other);
        emit(WindowsHost.EVENT_TIME_CHANGED);
        until(() -> delivered.get() == 1, "actual time event survives reentrant removal and failed callback");
        check(reentered.get() == 1 && late.get() == 0 && SystemServer.failures.get() == 1,
                "copy-on-write delivery isolates registration changes and reports listener failure");
        emit(WindowsHost.EVENT_TIME_CHANGED);
        until(() -> delivered.get() == 2 && late.get() == 1, "new registration receives the next real event");
        WindowsHost.removeListener(other); WindowsHost.removeListener(added);

        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        var values = new LinkedBlockingQueue<Integer>();
        WindowsHost.Listener blocked = events -> {
            if ((events & WindowsHost.EVENT_TIME_CHANGED) != 0) {
                entered.countDown();
                try { if (!release.await(8, TimeUnit.SECONDS)) throw new AssertionError("release timeout"); }
                catch (InterruptedException failure) { throw new AssertionError(failure); }
            } else values.add(events);
        };
        WindowsHost.addListener(blocked); emit(WindowsHost.EVENT_TIME_CHANGED);
        check(entered.await(8, TimeUnit.SECONDS), "listener can block after native event ownership released");
        int combined = WindowsHost.EVENT_RESUMED | WindowsHost.EVENT_POWER_STATUS;
        emit(WindowsHost.EVENT_RESUMED); emit(WindowsHost.EVENT_POWER_STATUS); release.countDown();
        until(() -> values.stream().anyMatch(value -> (value & combined) == combined),
                "native pending bits coalesce a burst without losing resume or power");
        WindowsHost.removeListener(blocked);
        check(dispatcher() == owner && stats()[0] == 1, "last listener removal retains the shared server owner");

        int[] before = stats();
        var bean = ManagementFactory.getThreadMXBean();
        long cpu = bean.isThreadCpuTimeSupported() ? bean.getThreadCpuTime(owner.threadId()) : -1;
        Thread.sleep(TimeUnit.SECONDS.toMillis(idleSeconds));
        int[] after = stats();
        long cpuAfter = bean.isThreadCpuTimeSupported() ? bean.getThreadCpuTime(owner.threadId()) : -1;
        System.out.println("Idle evidence: seconds=" + idleSeconds + " empty-wakes=" + (after[2] - before[2])
                + " actual-event-wakes=" + (after[3] - before[3]) + " dispatcher-cpu-ns="
                + (cpu >= 0 && cpuAfter >= 0 ? cpuAfter - cpu : -1));
        check(after[2] == before[2], "event dispatcher has no one-minute empty timeout wake");
        check(dispatcher() == owner, "server event delivery owner survives idle interval");
    }
    public static void main(String[] args) throws Exception {
        if (!args[1].equals("unavailable")) System.load(args[0]);
        switch (args[1]) {
            case "unavailable" -> unavailable();
            case "deadline" -> deadline();
            case "dispatch" -> dispatch(Integer.parseInt(args[2]));
            default -> throw new AssertionError(args[1]);
        }
        System.out.println("WindowsHost " + args[1] + " contracts passed: " + checks);
    }
}

final class SystemServer {
    static volatile boolean window;
    static final AtomicInteger failures = new AtomicInteger();
    static void log(String message) {
        if (message.contains("from a hidden window 0x")) window = true;
        if (message.contains("Windows event listener failed")) failures.incrementAndGet();
        System.out.println(message);
    }
}
