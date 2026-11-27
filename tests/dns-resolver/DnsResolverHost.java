/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.os.CancellationSignal;
import android.system.ErrnoException;
import android.system.OsConstants;
import com.gesher.net.WindowsNetworks;
import java.io.ByteArrayOutputStream;
import java.lang.management.ManagementFactory;
import java.lang.reflect.Field;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Set;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;

/** Complete production owner with controllable native and application-executor boundaries. */
public final class DnsResolverHost {
    private static final DnsResolver DNS = DnsResolver.getInstance();
    private static final Executor DIRECT = Runnable::run;
    private static final AtomicInteger CHECKS = new AtomicInteger();
    private static final AtomicReference<Throwable> ASYNC_FAILURE = new AtomicReference<>();
    private static final Object LOCK = optionalLock();
    private static final InetAddress V4 = address(new byte[]{1, 2, 3, 4});
    private static final InetAddress V6 = address(new byte[16]);
    private static final InetAddress[] SMALL = {V4, V6};
    private static final long BYTE_BUDGET = 8L * 1024 * 1024;

    static void check(boolean value, String message) {
        CHECKS.incrementAndGet();
        if (!value) throw new AssertionError(message);
    }

    static InetAddress address(byte[] raw) {
        try { return InetAddress.getByAddress(raw); }
        catch (UnknownHostException impossible) { throw new AssertionError(impossible); }
    }

    static Object field(String name) throws Exception {
        Field field = DnsResolver.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(null);
    }

    static Object optionalLock() {
        try { return field("sLock"); }
        catch (Exception baselineWithoutOwner) { return null; }
    }

    record Snapshot(int requests, long inputBytes, long answerBytes, int lookupQueue,
            int completionQueue) {}

    static Snapshot snapshot() {
        try {
            synchronized (LOCK) {
                return new Snapshot(((Set<?>) field("sRequests")).size(),
                        (Long) field("sInputBytes"), (Long) field("sAnswerBytes"),
                        ((ThreadPoolExecutor) field("sLookups")).getQueue().size(),
                        ((ThreadPoolExecutor) field("sDispatchers")).getQueue().size());
            }
        } catch (Exception failure) { throw new AssertionError(failure); }
    }

    static void until(BooleanSupplier condition, String message) throws Exception {
        until(condition, message, System.nanoTime() + TimeUnit.SECONDS.toNanos(8));
    }

    static void until(BooleanSupplier condition, String message, long end) throws Exception {
        // A state such as TIMED_WAITING can change as soon as it is observed.
        // Preserve that successful observation instead of sampling it again.
        boolean observed = condition.getAsBoolean();
        while (!observed && System.nanoTime() < end) {
            Thread.sleep(2);
            observed = condition.getAsBoolean();
        }
        Throwable failure = ASYNC_FAILURE.get();
        if (failure != null) throw new AssertionError("asynchronous fixture failure", failure);
        check(observed, message);
    }

    static void drain() throws Exception {
        until(() -> snapshot().requests == 0, "all actual lookup/executor/callback owners retire");
        Snapshot state = snapshot();
        check(state.inputBytes == 0 && state.answerBytes == 0, "byte credits return to zero");
        check(state.lookupQueue == 0 && state.completionQueue == 0, "internal queues empty");
        ScheduledThreadPoolExecutor deadlines = (ScheduledThreadPoolExecutor) field("sDeadlines");
        until(() -> deadlines.getQueue().isEmpty(), "completed deadline tasks removed");
    }

    static void backend(WindowsNetworks.Backend backend) {
        check(WindowsNetworks.active.get() == 0, "previous backend calls have actually returned");
        WindowsNetworks.calls.set(0);
        WindowsNetworks.peak.set(0);
        WindowsNetworks.backend = backend;
    }

    static void awaitUninterruptibly(CountDownLatch latch) {
        boolean interrupted = false;
        while (true) {
            try { latch.await(); break; }
            catch (InterruptedException ignored) { interrupted = true; }
        }
        if (interrupted) Thread.currentThread().interrupt();
    }

    static void outsideLock() {
        check(LOCK == null || !Thread.holdsLock(LOCK), "application callback outside resolver lock");
    }

    static final class Result<T> implements DnsResolver.Callback<T> {
        final CountDownLatch done = new CountDownLatch(1);
        final AtomicInteger calls = new AtomicInteger();
        volatile T value;
        volatile int rcode = -1;
        volatile DnsResolver.DnsException error;
        volatile Thread thread;
        @Override public void onAnswer(T answer, int code) {
            try { outsideLock(); thread = Thread.currentThread(); value = answer; rcode = code; calls.incrementAndGet(); }
            catch (Throwable failure) { ASYNC_FAILURE.compareAndSet(null, failure); }
            finally { done.countDown(); }
        }
        @Override public void onError(DnsResolver.DnsException failure) {
            try { outsideLock(); thread = Thread.currentThread(); error = failure; calls.incrementAndGet(); }
            catch (Throwable bug) { ASYNC_FAILURE.compareAndSet(null, bug); }
            finally { done.countDown(); }
        }
        void await() throws Exception {
            check(done.await(8, TimeUnit.SECONDS), "callback delivered");
            check(ASYNC_FAILURE.get() == null, "callback assertions passed");
            check(calls.get() == 1, "exactly one terminal callback");
        }
    }

    static final class Counts<T> implements DnsResolver.Callback<T> {
        final AtomicInteger answers = new AtomicInteger(), errors = new AtomicInteger();
        volatile int errno;
        @Override public void onAnswer(T value, int rcode) {
            try { outsideLock(); answers.incrementAndGet(); }
            catch (Throwable bug) { ASYNC_FAILURE.compareAndSet(null, bug); }
        }
        @Override public void onError(DnsResolver.DnsException error) {
            try {
                outsideLock();
                if (error.getCause() instanceof ErrnoException cause) errno = cause.errno;
                errors.incrementAndGet();
            } catch (Throwable bug) { ASYNC_FAILURE.compareAndSet(null, bug); }
        }
    }

    static final class HeldExecutor implements Executor {
        final BlockingQueue<Runnable> tasks = new LinkedBlockingQueue<>();
        @Override public void execute(Runnable task) { outsideLock(); tasks.add(task); }
        void consume() { Runnable task; while ((task = tasks.poll()) != null) task.run(); }
    }

    static byte[] question(int id, String domain, int type) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(id >> 8); out.write(id); out.write(1); out.write(0);
        out.write(0); out.write(1);
        for (int i = 0; i < 6; i++) out.write(0);
        for (String label : domain.split("\\.")) {
            out.write(label.length());
            for (char c : label.toCharArray()) out.write(c);
        }
        out.write(0); out.write(type >> 8); out.write(type); out.write(0); out.write(1);
        return out.toByteArray();
    }

    static void query(String name, Executor executor, CancellationSignal signal,
            DnsResolver.Callback<? super List<InetAddress>> callback) {
        DNS.query(null, name, 0, executor, signal, callback);
    }

    /** Public API only: this same assertion rejects the original one-thread-per-call class. */
    static void burstControl() throws Exception {
        CountDownLatch gate = new CountDownLatch(1);
        backend((network, name) -> { awaitUninterruptibly(gate); return SMALL; });
        Counts<List<InetAddress>> callbacks = new Counts<>();
        try {
            for (int i = 0; i < 64; i++) query("bounded.test", DIRECT, null, callbacks);
            until(() -> WindowsNetworks.calls.get() >= 32, "32 real lookup boundaries reached");
            Thread.sleep(150);
            check(WindowsNetworks.peak.get() <= 32, "public API burst never starts more than 32 lookups");
        } finally { gate.countDown(); }
        until(() -> callbacks.answers.get() == 64, "all 64 accepted calls eventually complete");
        if (LOCK != null) drain();
    }

    /** Allocations on the API caller expose a clone even when no lookup/callback ever runs. */
    static void copyControl() throws Exception {
        backend((network, name) -> SMALL);
        byte[] oversized = new byte[16 * 1024 * 1024];
        CancellationSignal cancelled = new CancellationSignal();
        cancelled.cancel();
        Counts<byte[]> callbacks = new Counts<>();
        for (int i = 0; i < 2; i++) DNS.rawQuery(null, oversized, 0, DIRECT, cancelled, callbacks);
        var bean = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
        check(bean.isThreadAllocatedMemorySupported(), "allocation counter available on test JDK");
        bean.setThreadAllocatedMemoryEnabled(true);
        long id = Thread.currentThread().threadId();
        long before = bean.getThreadAllocatedBytes(id);
        for (int i = 0; i < 8; i++) DNS.rawQuery(null, oversized, 0, DIRECT, cancelled, callbacks);
        long allocated = bean.getThreadAllocatedBytes(id) - before;
        System.out.println("Pre-cancelled raw calls allocated " + allocated + " caller-thread bytes");
        check(allocated < 1024 * 1024, "cancelled raw calls admit before cloning 128 MiB of input");
        check(WindowsNetworks.calls.get() == 0 && callbacks.answers.get() == 0
                && callbacks.errors.get() == 0, "pre-cancelled calls remain silent");
    }

    static void burst(int total) throws Exception {
        CountDownLatch gate = new CountDownLatch(1);
        backend((network, name) -> { awaitUninterruptibly(gate); return SMALL; });
        Counts<List<InetAddress>> callbacks = new Counts<>();
        List<CancellationSignal> signals = new ArrayList<>();
        int accepted = Math.min(total, 256), running = Math.min(total, 32);
        try {
            for (int i = 0; i < total; i++) {
                CancellationSignal signal = new CancellationSignal(); signals.add(signal);
                query("burst.test", DIRECT, signal, callbacks);
            }
            until(() -> WindowsNetworks.calls.get() == running, "bounded workers enter backend");
            Snapshot state = snapshot();
            check(state.requests == accepted, "admission count for burst " + total);
            check(state.lookupQueue == accepted - running, "lookup queue bounded by accepted requests");
            check(WindowsNetworks.peak.get() == running, "active backend peak for burst " + total);
            check(callbacks.errors.get() == total - accepted, "every overloaded call receives an error");
            if (total > accepted) check(callbacks.errno == OsConstants.EAGAIN, "overload uses EAGAIN");
            for (CancellationSignal signal : signals) signal.cancel();
            check(snapshot().requests == running, "active cancellation retains uncancelable lookups");
            check(snapshot().lookupQueue == 0, "queued cancellation unlinks before native lookup");
            check(callbacks.answers.get() == 0, "cancellation never fabricates an answer");
        } finally { gate.countDown(); }
        drain();
        check(WindowsNetworks.calls.get() == running, "cancelled queued requests never resolve");
    }

    static void inputBytes() throws Exception {
        CountDownLatch gate = new CountDownLatch(1);
        backend((network, name) -> { awaitUninterruptibly(gate); return SMALL; });
        byte[] raw = Arrays.copyOf(question(7, "bytes.test", DnsResolver.TYPE_A), 65535);
        List<CancellationSignal> signals = new ArrayList<>();
        Counts<byte[]> callbacks = new Counts<>();
        try {
            for (int i = 0; i < 140; i++) {
                CancellationSignal signal = new CancellationSignal(); signals.add(signal);
                DNS.rawQuery(null, raw, 0, DIRECT, signal, callbacks);
            }
            until(() -> WindowsNetworks.calls.get() == 32, "raw workers entered backend");
            Snapshot state = snapshot();
            check(state.requests == 128 && state.inputBytes == 128L * raw.length,
                    "8 MiB input credit limits full-sized raw requests before copying");
            check(callbacks.errors.get() == 12 && callbacks.errno == OsConstants.EAGAIN,
                    "raw byte overload receives one error per rejected request");
            Result<byte[]> tooLarge = new Result<>();
            DNS.rawQuery(null, new byte[65536], 0, DIRECT, null, tooLarge);
            tooLarge.await();
            check(((ErrnoException) tooLarge.error.getCause()).errno == OsConstants.EMSGSIZE,
                    "oversized individual raw request gets EMSGSIZE without admission");
            check(tooLarge.thread == Thread.currentThread(),
                    "immediate admission failure invokes supplied executor on the API caller");
            check(snapshot().inputBytes == state.inputBytes, "rejected input owns no copied bytes");
            for (CancellationSignal signal : signals) signal.cancel();
            check(snapshot().inputBytes == 32L * raw.length,
                    "active raw copies remain charged until the backend returns");
        } finally { gate.countDown(); }
        drain();
    }

    static void inputMutationAndProtocol() throws Exception {
        CountDownLatch gate = new CountDownLatch(1);
        backend((network, name) -> { awaitUninterruptibly(gate); return SMALL; });
        byte[] raw = question(0x1234, "copy.test", DnsResolver.TYPE_A);
        Result<byte[]> copied = new Result<>();
        DNS.rawQuery(new Network(29), raw, 7, DIRECT, null, copied);
        until(() -> WindowsNetworks.calls.get() == 1, "native lookup owns admitted input");
        Arrays.fill(raw, (byte) 0);
        gate.countDown(); copied.await(); drain();
        check(copied.value[0] == 0x12 && copied.value[1] == 0x34, "raw query snapshot preserves ID");
        check(copied.rcode == 0 && copied.value[7] == 1, "A query filters out IPv6");
        check(WindowsNetworks.lastNetwork.get() == 29 && WindowsNetworks.lastDomain.equals("copy.test"),
                "explicit network and original query name reach existing backend");

        backend((network, name) -> SMALL);
        Result<List<InetAddress>> list = new Result<>();
        query("addresses.test", DIRECT, null, list); list.await(); drain();
        check(list.value.equals(List.of(V4, V6)) && WindowsNetworks.lastNetwork.get() == 17,
                "untyped query and process network selection preserved");
        Result<List<InetAddress>> only6 = new Result<>();
        DNS.query(null, "addresses.test", DnsResolver.TYPE_AAAA, 0, DIRECT, null, only6);
        only6.await(); drain();
        check(only6.value.equals(List.of(V6)), "AAAA filters IPv4");

        Result<byte[]> otherType = new Result<>();
        DNS.rawQuery(null, "addresses.test", 1, 15, 0, DIRECT, null, otherType);
        otherType.await(); drain();
        check(otherType.rcode == 4 && otherType.value[7] == 0, "unsupported raw type remains NOTIMP");
        byte[] malformed = new byte[13]; malformed[12] = 63;
        Result<byte[]> parse = new Result<>();
        DNS.rawQuery(null, malformed, 0, DIRECT, null, parse);
        parse.await(); drain();
        check(parse.error.code == DnsResolver.ERROR_PARSE, "truncated name remains a parse error");

        backend((network, name) -> { throw new UnknownHostException(name); });
        Result<List<InetAddress>> missing = new Result<>();
        query("missing.test", DIRECT, null, missing); missing.await(); drain();
        check(missing.value.isEmpty() && missing.rcode == 3, "missing name remains empty NXDOMAIN");
        backend((network, name) -> new InetAddress[4097]);
        Result<List<InetAddress>> huge = new Result<>();
        query("huge.test", DIRECT, null, huge); huge.await(); drain();
        check(((ErrnoException) huge.error.getCause()).errno == OsConstants.ENOBUFS,
                "native oversized address result rejected before list copying");
        InetAddress[] maximum = new InetAddress[4096]; Arrays.fill(maximum, V4);
        backend((network, name) -> maximum);
        Result<byte[]> longWire = new Result<>();
        DNS.rawQuery(null, question(9, "wire.test", DnsResolver.TYPE_A), 0, DIRECT, null, longWire);
        longWire.await(); drain();
        check(((ErrnoException) longWire.error.getCause()).errno == OsConstants.EMSGSIZE,
                "encoded raw reply size is checked before its output allocation");
        int nullChecks = 0;
        try { DNS.rawQuery(null, (byte[]) null, 0, DIRECT, null, new Result<byte[]>()); }
        catch (NullPointerException expected) { nullChecks++; }
        try { query(null, DIRECT, null, new Result<List<InetAddress>>()); }
        catch (NullPointerException expected) { nullChecks++; }
        try { query("null.test", null, null, new Result<List<InetAddress>>()); }
        catch (NullPointerException expected) { nullChecks++; }
        try { query("null.test", DIRECT, null, null); }
        catch (NullPointerException expected) { nullChecks++; }
        check(nullChecks == 4, "public null-argument exceptions remain synchronous");
        drain();
    }

    static void retainedCallbacks() throws Exception {
        backend((network, name) -> SMALL);
        HeldExecutor held = new HeldExecutor();
        List<CancellationSignal> signals = new ArrayList<>();
        Counts<List<InetAddress>> callbacks = new Counts<>();
        for (int i = 0; i < 256; i++) {
            CancellationSignal signal = new CancellationSignal(); signals.add(signal);
            query("held.test", held, signal, callbacks);
        }
        until(() -> held.tasks.size() == 256, "all accepted completions handed to caller executor");
        check(snapshot().requests == 256, "never-run callback wrappers retain admission");
        for (CancellationSignal signal : signals) signal.cancel();
        check(snapshot().requests == 256 && snapshot().answerBytes == 0,
                "cancellation clears payload without recycling retained callback wrappers");
        for (int i = 0; i < 1024; i++) query("rejected.test", DIRECT, null, callbacks);
        check(callbacks.errors.get() == 1024 && snapshot().requests == 256,
                "rejection notifications remain outside the accepted ownership budget");
        held.consume(); drain();
        check(callbacks.answers.get() == 0, "cancelled executor-held callbacks remain silent");
    }

    static void retainedAnswers() throws Exception {
        InetAddress[] large = new InetAddress[4096]; Arrays.fill(large, V4);
        backend((network, name) -> large);
        HeldExecutor held = new HeldExecutor();
        List<CancellationSignal> signals = new ArrayList<>();
        Counts<List<InetAddress>> callbacks = new Counts<>();
        for (int i = 0; i < 48; i++) {
            CancellationSignal signal = new CancellationSignal(); signals.add(signal);
            query("large.test", held, signal, callbacks);
        }
        until(() -> held.tasks.size() == 15 && WindowsNetworks.calls.get() >= 47,
                "reply credit bounds external completions and waiting producers");
        Snapshot state = snapshot();
        check(state.answerBytes == 15 * (128L * 4096 + 256) && state.answerBytes <= BYTE_BUDGET,
                "retained answer bytes stay within 8 MiB");
        check(WindowsNetworks.calls.get() == 47 && state.lookupQueue == 1,
                "at most 32 lookup producers hold one additional bounded result");
        for (CancellationSignal signal : signals) signal.cancel();
        // Cancelling one delivered answer frees credit before later signals are
        // cancelled; another producer may legitimately hand off in that interval.
        until(() -> snapshot().requests == held.tasks.size()
                && snapshot().lookupQueue == 0, "only actually handed callback wrappers remain");
        check(snapshot().requests >= 15 && snapshot().requests <= 48,
                "cancellation cannot recycle or multiply executor-held wrappers");
        check(snapshot().answerBytes == 0, "cancelled queued answers release bulky storage");
        held.consume(); drain();
        check(callbacks.answers.get() == 0 && callbacks.errors.get() == 0,
                "cancelled answer-backpressure work has no terminal delivery");
    }

    static void executorLifetimes() throws Exception {
        backend((network, name) -> SMALL);
        CountDownLatch gate = new CountDownLatch(1);
        AtomicInteger entered = new AtomicInteger();
        Executor blocked = task -> { outsideLock(); entered.incrementAndGet(); awaitUninterruptibly(gate); task.run(); };
        Counts<List<InetAddress>> callbacks = new Counts<>();
        List<CancellationSignal> signals = new ArrayList<>();
        for (int i = 0; i < 8; i++) {
            CancellationSignal signal = new CancellationSignal(); signals.add(signal);
            query("dispatch.test", blocked, signal, callbacks);
        }
        until(() -> entered.get() == 4 && snapshot().completionQueue == 4,
                "four dispatch workers contain blocking execute calls");
        check(WindowsNetworks.active.get() == 0 && snapshot().requests == 8,
                "blocking application executors do not occupy native lookup workers");
        for (CancellationSignal signal : signals) signal.cancel();
        check(snapshot().requests == 4 && snapshot().completionQueue == 0,
                "only actual blocking execute owners remain after cancellation");
        gate.countDown(); drain();
        check(callbacks.answers.get() == 0, "cancellation suppresses delayed execute delivery");

        CountDownLatch afterRun = new CountDownLatch(1), executorReturning = new CountDownLatch(1);
        Result<List<InetAddress>> inline = new Result<>();
        query("after-run.test", task -> {
            task.run(); afterRun.countDown(); awaitUninterruptibly(executorReturning);
        }, null, inline);
        check(afterRun.await(8, TimeUnit.SECONDS), "inline callback returned inside execute");
        inline.await();
        check(snapshot().requests == 1 && snapshot().answerBytes == 0,
                "execute ownership persists even after its inline callback returns");
        executorReturning.countDown(); drain();

        Counts<List<InetAddress>> rejected = new Counts<>();
        query("rejected-executor.test", task -> { throw new RejectedExecutionException("closed"); },
                null, rejected);
        drain();
        check(rejected.answers.get() == 0 && rejected.errors.get() == 0,
                "a rejecting executor does not trigger fallback delivery on another thread");

        HeldExecutor ambiguous = new HeldExecutor();
        query("enqueue-throw.test", task -> {
            ambiguous.execute(task); throw new IllegalStateException("enqueued then failed");
        }, null, rejected);
        until(() -> ambiguous.tasks.size() == 1 && snapshot().answerBytes == 0,
                "ambiguous throwing executor clears payload");
        check(snapshot().requests == 1, "possibly enqueued wrapper remains charged after execute throws");
        ambiguous.consume(); drain();
        check(rejected.answers.get() == 0 && rejected.errors.get() == 0,
                "failed executor's retained wrapper cannot deliver stale data");

        Result<List<InetAddress>> duplicate = new Result<>();
        query("duplicate.test", task -> { task.run(); task.run(); }, null, duplicate);
        duplicate.await(); drain();
        check(duplicate.calls.get() == 1, "malformed duplicate executor invocation is terminal once");

        AtomicInteger throwingCalls = new AtomicInteger();
        DnsResolver.Callback<List<InetAddress>> throwing = new DnsResolver.Callback<>() {
            @Override public void onAnswer(List<InetAddress> value, int code) {
                throwingCalls.incrementAndGet(); throw new AssertionError("application callback");
            }
            @Override public void onError(DnsResolver.DnsException error) { throw new AssertionError(error); }
        };
        query("throwing-callback.test", DIRECT, null, throwing); drain();
        check(throwingCalls.get() == 1, "throwing inline callback releases its result in finally");
        Result<List<InetAddress>> afterThrow = new Result<>();
        query("after-throw.test", task -> { task.run(); throw new AssertionError("after callback"); },
                null, afterThrow);
        afterThrow.await(); drain();

        CountDownLatch inCallback = new CountDownLatch(1), callbackRelease = new CountDownLatch(1);
        CancellationSignal activeSignal = new CancellationSignal();
        DnsResolver.Callback<List<InetAddress>> active = new DnsResolver.Callback<>() {
            @Override public void onAnswer(List<InetAddress> value, int code) {
                outsideLock(); inCallback.countDown(); awaitUninterruptibly(callbackRelease);
                check(value.equals(List.of(V4, V6)), "running callback still owns its result");
            }
            @Override public void onError(DnsResolver.DnsException error) { ASYNC_FAILURE.set(error); }
        };
        query("active-callback.test", DIRECT, activeSignal, active);
        check(inCallback.await(8, TimeUnit.SECONDS), "application callback started");
        long charged = snapshot().answerBytes;
        activeSignal.cancel();
        check(snapshot().requests == 1 && charged > 0 && snapshot().answerBytes == charged,
                "cancelling an already running callback cannot release its live answer");
        callbackRelease.countDown(); drain();

        CountDownLatch reentered = new CountDownLatch(1);
        Result<List<InetAddress>> nested = new Result<>();
        query("reentry.test", DIRECT, null, new DnsResolver.Callback<>() {
            @Override public void onAnswer(List<InetAddress> value, int code) {
                try {
                    outsideLock();
                    synchronized (reentered) {
                        query("nested.test", DIRECT, null, nested);
                        reentered.countDown();
                    }
                } catch (Throwable failure) { ASYNC_FAILURE.set(failure); }
            }
            @Override public void onError(DnsResolver.DnsException error) { ASYNC_FAILURE.set(error); }
        });
        check(reentered.await(8, TimeUnit.SECONDS), "reentrant submit returns without callback-capacity wait");
        nested.await(); drain();
    }

    static void cancellationOwnership() throws Exception {
        CountDownLatch gate = new CountDownLatch(1);
        backend((network, name) -> { awaitUninterruptibly(gate); return SMALL; });
        CancellationSignal shared = new CancellationSignal();
        Counts<List<InetAddress>> callbacks = new Counts<>();
        for (int i = 0; i < 64; i++) query("shared-signal.test", DIRECT, shared, callbacks);
        until(() -> WindowsNetworks.calls.get() == 32, "shared signal owns active and queued work");
        Thread cancel = new Thread(shared::cancel);
        cancel.start(); cancel.join(2000);
        check(!cancel.isAlive(), "cancel listener does not detach itself and deadlock");
        check(snapshot().requests == 32 && snapshot().lookupQueue == 0,
                "last registration cancels every current request using the same signal");
        gate.countDown(); drain();
        check(callbacks.answers.get() == 0, "shared-signal cancellation suppresses all accepted delivery");

        backend((network, name) -> SMALL);
        CancellationSignal reused = new CancellationSignal();
        HeldExecutor first = new HeldExecutor(), second = new HeldExecutor();
        query("older.test", first, reused, callbacks);
        until(() -> first.tasks.size() == 1, "older completion handed to application executor");
        query("newer.test", second, reused, callbacks);
        until(() -> second.tasks.size() == 1, "newer request installs current listener");
        first.consume();
        until(() -> snapshot().requests == 1, "older request completes without clearing new listener");
        reused.cancel(); second.consume(); drain();
        check(callbacks.answers.get() == 1, "newer cancelled callback stays silent after older completion");

        // Race registration/admission against the real AOSP signal lock and callback.
        for (int round = 0; round < 200; round++) {
            CancellationSignal signal = new CancellationSignal();
            Result<List<InetAddress>> result = new Result<>();
            Thread producer = new Thread(() -> query("race.test", DIRECT, signal, result));
            Thread canceller = new Thread(signal::cancel);
            producer.start(); canceller.start(); producer.join(2000); canceller.join(2000);
            check(!producer.isAlive() && !canceller.isAlive(), "submit/cancel race terminates");
            check(result.calls.get() <= 1, "submit/cancel race cannot duplicate a callback");
        }
        drain();
    }

    static void deadlines() throws Exception {
        CountDownLatch gate = new CountDownLatch(1), callbacks = new CountDownLatch(33);
        backend((network, name) -> { awaitUninterruptibly(gate); return SMALL; });
        AtomicInteger errors = new AtomicInteger(), answers = new AtomicInteger();
        long started = System.nanoTime();
        DnsResolver.Callback<List<InetAddress>> callback = new DnsResolver.Callback<>() {
            @Override public void onAnswer(List<InetAddress> answer, int rcode) {
                answers.incrementAndGet(); callbacks.countDown();
            }
            @Override public void onError(DnsResolver.DnsException error) {
                try {
                    outsideLock();
                    check(error.code == DnsResolver.ERROR_SYSTEM
                            && ((ErrnoException) error.getCause()).errno == OsConstants.ETIMEDOUT,
                            "deadline delivers an actual timeout error");
                    errors.incrementAndGet();
                } catch (Throwable failure) { ASYNC_FAILURE.set(failure); }
                finally { callbacks.countDown(); }
            }
        };
        try {
            for (int i = 0; i < 33; i++) query("deadline.test", DIRECT, null, callback);
            until(() -> WindowsNetworks.calls.get() == 32, "deadline case saturates native lane");
            check(callbacks.await(40, TimeUnit.SECONDS), "actual 30-second queued and active deadlines fire");
            check(errors.get() == 33 && answers.get() == 0, "one timeout per admitted query");
            // The callback's latch is signalled before its finally and the inline
            // Executor return. Those real owners must retire before observing 32.
            awaitQueuedTimeoutRetirement(started + TimeUnit.SECONDS.toNanos(40));
            long elapsed = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started);
            check(elapsed >= 29_000 && elapsed < 40_000, "production monotonic deadline duration");
            check(WindowsNetworks.active.get() == 32 && WindowsNetworks.calls.get() == 32,
                    "timeouts do not start extra native workers or cancel them by assertion");
            System.out.println("Actual query deadline: " + elapsed + " ms; 32 native owners retained");
        } finally { gate.countDown(); }
        drain();
        check(errors.get() == 33 && answers.get() == 0, "late native success cannot follow timeout delivery");
        backend((network, name) -> SMALL);
        Result<List<InetAddress>> recovered = new Result<>();
        query("capacity-returned.test", DIRECT, null, recovered);
        recovered.await(); drain();
    }

    static void awaitQueuedTimeoutRetirement(long end) throws Exception {
        until(() -> {
            Snapshot state = snapshot();
            return state.requests == 32 && state.lookupQueue == 0;
        }, "queued timeout unlinks; timed-out uncancelable lookups keep their slots", end);
    }

    static void deadlineRetirement() throws Exception {
        AtomicInteger observations = new AtomicInteger();
        until(() -> observations.getAndIncrement() == 0,
                "a successful transient observation is preserved");
        check(observations.get() == 1, "a successful wait samples its predicate only once");
        CountDownLatch nativeReturn = new CountDownLatch(1), executorReturn = new CountDownLatch(1);
        CountDownLatch queuedCallbackReturned = new CountDownLatch(1), observationDone = new CountDownLatch(1);
        AtomicReference<Throwable> observationFailure = new AtomicReference<>();
        Counts<List<InetAddress>> callbacks = new Counts<>();
        backend((network, name) -> { awaitUninterruptibly(nativeReturn); return SMALL; });
        Thread observer = new Thread(() -> {
            try { awaitQueuedTimeoutRetirement(System.nanoTime() + TimeUnit.SECONDS.toNanos(8)); }
            catch (Throwable failure) { observationFailure.set(failure); }
            finally { observationDone.countDown(); }
        }, "DnsDeadline-retirement-observer");
        try {
            for (int i = 0; i < 32; ++i) query("retirement-active.test", DIRECT, null, callbacks);
            until(() -> WindowsNetworks.active.get() == 32, "retirement control owns 32 native calls");
            query("retirement-queued.test", task -> {
                task.run();
                queuedCallbackReturned.countDown();
                awaitUninterruptibly(executorReturn);
            }, null, callbacks);
            List<?> requests;
            synchronized (LOCK) { requests = new ArrayList<>((Set<?>) field("sRequests")); }
            check(requests.size() == 33, "retirement control has one actual queued request");
            // Invoke the real timer transition under a controlled schedule. The
            // separate deadline case still waits for the unmodified 30-second timer.
            for (Object request : requests) {
                var timeout = request.getClass().getDeclaredMethod("timeout");
                timeout.setAccessible(true);
                timeout.invoke(request);
            }
            check(queuedCallbackReturned.await(8, TimeUnit.SECONDS), "queued callback actually returned");
            until(() -> callbacks.errors.get() == 33, "all controlled real timeout callbacks delivered");
            Snapshot held = snapshot();
            check(held.lookupQueue == 0 && held.completionQueue == 0,
                    "controlled queued timeout actually unlinks its lookup and completion tasks");
            check(held.requests == 33,
                    "returned callback still charges its blocked Executor and 32 native owners");
            check(WindowsNetworks.active.get() == 32 && WindowsNetworks.calls.get() == 32,
                    "controlled timeouts cannot retire or replace actual native calls");
            observer.start();
            until(() -> observationDone.getCount() == 0 || observer.getState() == Thread.State.TIMED_WAITING,
                    "retirement observation reaches its bounded wait");
            if (observationFailure.get() != null)
                throw new AssertionError("callback notification is not Executor retirement", observationFailure.get());
            check(observationDone.getCount() == 1, "retirement observer cannot pass while Executor is held");
            executorReturn.countDown();
            check(observationDone.await(8, TimeUnit.SECONDS), "retirement observer resumes after real Executor return");
            check(observationFailure.get() == null, "real Executor return satisfies the original ownership predicate");
            check(snapshot().requests == 32 && WindowsNetworks.active.get() == 32,
                    "only completed queued ownership retires before native calls return");
        } finally {
            executorReturn.countDown(); nativeReturn.countDown();
            observer.join(9000);
        }
        check(!observer.isAlive(), "retirement control observer actually terminates");
        drain();
        check(callbacks.errors.get() == 33 && callbacks.answers.get() == 0,
                "late native completion cannot duplicate the controlled terminal callbacks");
    }

    static void idleRetirement() throws Exception {
        for (String name : List.of("sLookups", "sDispatchers", "sDeadlines")) {
            ThreadPoolExecutor pool = (ThreadPoolExecutor) field(name);
            check(pool.allowsCoreThreadTimeOut() && pool.getKeepAliveTime(TimeUnit.SECONDS) == 30,
                    "production worker idle timeout is thirty seconds: " + name);
            // Exercise the real executor's same retirement mechanism without another 30-second wait.
            pool.setKeepAliveTime(100, TimeUnit.MILLISECONDS);
            until(() -> pool.getPoolSize() == 0, "idle threads retire: " + name);
        }
    }

    static void retireIdlePool(ThreadPoolExecutor pool) throws Exception {
        long keepAlive = pool.getKeepAliveTime(TimeUnit.MILLISECONDS);
        pool.setKeepAliveTime(100, TimeUnit.MILLISECONDS);
        until(() -> pool.getPoolSize() == 0, "fault fixture starts with no idle worker");
        pool.setKeepAliveTime(keepAlive, TimeUnit.MILLISECONDS);
    }

    static void startupFailures() throws Exception {
        drain();
        backend((network, name) -> SMALL);
        ScheduledThreadPoolExecutor timers = (ScheduledThreadPoolExecutor) field("sDeadlines");
        ThreadFactory timerFactory = timers.getThreadFactory();
        retireIdlePool(timers);
        Counts<List<InetAddress>> callbacks = new Counts<>();
        int rejected = 0;
        timers.setThreadFactory(task -> { throw new OutOfMemoryError("controlled timer Thread.start failure"); });
        try {
            for (int i = 0; i < 1024; i++) {
                try { query("timer-start.test", DIRECT, null, callbacks); }
                catch (OutOfMemoryError expected) { rejected++; }
                check(snapshot().requests == 0 && timers.getQueue().isEmpty(),
                        "failed timer startup rolls back the actual already-enqueued future");
            }
        } finally { timers.setThreadFactory(timerFactory); }
        check(rejected == 1024 && WindowsNetworks.calls.get() == 0,
                "timer startup failure cannot retain requests or start native calls");
        Result<List<InetAddress>> afterTimerFailure = new Result<>();
        query("timer-recovered.test", DIRECT, null, afterTimerFailure);
        afterTimerFailure.await(); drain();

        ThreadPoolExecutor dispatch = (ThreadPoolExecutor) field("sDispatchers");
        ThreadFactory factory = dispatch.getThreadFactory();
        AtomicInteger attempts = new AtomicInteger();
        AtomicReference<Throwable> uncaught = new AtomicReference<>();
        Thread.UncaughtExceptionHandler handler = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> uncaught.set(error));
        retireIdlePool(dispatch);
        CountDownLatch lookupRelease = new CountDownLatch(1);
        backend((network, name) -> { awaitUninterruptibly(lookupRelease); return SMALL; });
        dispatch.setThreadFactory(task -> {
            if (attempts.incrementAndGet() == 1) {
                // Give an idle timer time to retire while completion startup is
                // delayed. An accepted owner must keep the existing timer alive
                // until its retry has been scheduled, even if a new one cannot start.
                try { Thread.sleep(250); }
                catch (InterruptedException error) { throw new AssertionError(error); }
                throw new OutOfMemoryError("controlled dispatcher Thread.start failure");
            }
            return factory.newThread(task);
        });
        try {
            Result<List<InetAddress>> result = new Result<>();
            query("dispatcher-retry.test", DIRECT, null, result);
            until(() -> WindowsNetworks.active.get() == 1, "accepted lookup has an existing deadline worker");
            timers.setKeepAliveTime(100, TimeUnit.MILLISECONDS);
            timers.setThreadFactory(task -> {
                throw new OutOfMemoryError("completion retry must not need a replacement timer worker");
            });
            lookupRelease.countDown();
            until(() -> attempts.get() == 1 && snapshot().completionQueue == 1,
                    "failed worker startup preserves one queued completion");
            check(snapshot().requests == 1, "completion retry retains its admission");
            result.await(); drain();
            check(attempts.get() == 2 && uncaught.get() == null,
                    "same completion recovers after one retry without losing a lookup worker");
            timers.setThreadFactory(timerFactory);
            timers.setKeepAliveTime(30, TimeUnit.SECONDS);
            backend((network, name) -> SMALL);

            // One existing worker is sufficient even if additional workers cannot start.
            attempts.set(0);
            dispatch.setThreadFactory(task -> {
                attempts.incrementAndGet();
                throw new OutOfMemoryError("controlled additional worker failure");
            });
            Result<List<InetAddress>> existing = new Result<>();
            query("existing-dispatcher.test", DIRECT, null, existing);
            existing.await(); drain();
            check(attempts.get() >= 1 && existing.calls.get() == 1,
                    "existing worker consumes the original queued task after growth failure");

            retireIdlePool(dispatch);
            attempts.set(0);
            CancellationSignal cancel = new CancellationSignal();
            query("cancel-retry.test", DIRECT, cancel, callbacks);
            until(() -> attempts.get() == 1 && snapshot().completionQueue == 1,
                    "pending retry is owned before cancellation");
            cancel.cancel(); drain();
            Thread.sleep(1100);
            check(attempts.get() == 1 && callbacks.answers.get() == 0
                    && callbacks.errors.get() == 0, "cancellation removes queued completion and retry timer");
            check(uncaught.get() == null, "controlled worker creation failures stay within owner recovery");
        } finally {
            lookupRelease.countDown();
            timers.setThreadFactory(timerFactory);
            timers.setKeepAliveTime(30, TimeUnit.SECONDS);
            dispatch.setThreadFactory(factory);
            Thread.setDefaultUncaughtExceptionHandler(handler);
        }
    }

    static void delayedDeadline() throws Exception {
        CountDownLatch timerStarted = new CountDownLatch(1), timerRelease = new CountDownLatch(1);
        CountDownLatch backendRelease = new CountDownLatch(1);
        ScheduledThreadPoolExecutor timers = (ScheduledThreadPoolExecutor) field("sDeadlines");
        timers.execute(() -> { timerStarted.countDown(); awaitUninterruptibly(timerRelease); });
        check(timerStarted.await(5, TimeUnit.SECONDS), "timer-delay fixture owns the real timer worker");
        backend((network, name) -> { awaitUninterruptibly(backendRelease); return SMALL; });
        Counts<List<InetAddress>> callbacks = new Counts<>();
        try {
            for (int i = 0; i < 33; i++) query("delayed-timer.test", DIRECT, null, callbacks);
            until(() -> WindowsNetworks.calls.get() == 32, "one query remains queued behind 32 native calls");
            Thread.sleep(30_100);
            backendRelease.countDown();
            until(() -> callbacks.errors.get() == 33, "lookup entry enforces deadlines without timer dispatch");
            check(WindowsNetworks.calls.get() == 32, "expired queued query never enters native backend");
            check(callbacks.answers.get() == 0 && callbacks.errno == OsConstants.ETIMEDOUT,
                    "expired queued and active work reports timeout exactly once");
        } finally {
            backendRelease.countDown(); timerRelease.countDown();
        }
        drain();
    }

    public static void main(String[] args) throws Exception {
        String mode = args.length == 0 ? "all" : args[0];
        if (mode.equals("burst")) burstControl();
        else if (mode.equals("copy")) copyControl();
        else if (mode.equals("deadline")) deadlines();
        else if (mode.equals("deadline-retirement")) deadlineRetirement();
        else if (mode.equals("delayed-deadline")) delayedDeadline();
        else if (mode.equals("recovery")) startupFailures();
        else {
            burstControl(); copyControl();
            for (int count : new int[]{1, 32, 256, 1024}) burst(count);
            inputBytes(); inputMutationAndProtocol();
            retainedCallbacks(); retainedAnswers(); executorLifetimes(); cancellationOwnership();
            startupFailures();
            idleRetirement();
        }
        check(ASYNC_FAILURE.get() == null, "all asynchronous fixture assertions passed");
        System.out.println("DnsResolver production " + mode + " contracts passed: " + CHECKS.get());
    }
}
