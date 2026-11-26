/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.net.DnsResolver;
import android.os.CancellationSignal;
import android.system.ErrnoException;
import android.system.OsConstants;
import java.lang.reflect.Field;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Real ART, real Windows localhost lookup and real Android cancellation; no resolver adapter. */
public final class DnsAdmissionTest {
    private static int checks;
    private static void require(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }

    private static int ownedRequests() throws Exception {
        Field lock = DnsResolver.class.getDeclaredField("sLock");
        Field requests = DnsResolver.class.getDeclaredField("sRequests");
        lock.setAccessible(true); requests.setAccessible(true);
        synchronized (lock.get(null)) { return ((Set<?>) requests.get(null)).size(); }
    }

    private static void drained() throws Exception {
        long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (ownedRequests() != 0 && System.nanoTime() - until < 0) Thread.sleep(1);
        require(ownedRequests() == 0, "real completion and execute owners retire");
    }

    private static void localhost(DnsResolver resolver) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<List<InetAddress>> answer = new AtomicReference<>();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        resolver.query(null, "localhost", DnsResolver.TYPE_A, 0, Runnable::run, null,
                new DnsResolver.Callback<List<InetAddress>>() {
                    @Override public void onAnswer(List<InetAddress> value, int rcode) {
                        if (rcode != 0) failure.set(new AssertionError("localhost rcode " + rcode));
                        answer.set(value); done.countDown();
                    }
                    @Override public void onError(DnsResolver.DnsException error) {
                        failure.set(error); done.countDown();
                    }
                });
        require(done.await(35, TimeUnit.SECONDS), "real localhost callback delivered");
        require(failure.get() == null, "real localhost failed: " + failure.get());
        require(answer.get().contains(InetAddress.getByAddress(new byte[]{127, 0, 0, 1})),
                "real Windows resolver returns loopback");
        drained();
    }

    public static void main(String[] args) throws Exception {
        DnsResolver resolver = DnsResolver.getInstance();
        localhost(resolver);
        LinkedBlockingQueue<Runnable> retained = new LinkedBlockingQueue<>();
        Executor held = retained::add;
        AtomicInteger answers = new AtomicInteger(), errors = new AtomicInteger();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        DnsResolver.Callback<List<InetAddress>> callback = new DnsResolver.Callback<List<InetAddress>>() {
            @Override public void onAnswer(List<InetAddress> value, int rcode) { answers.incrementAndGet(); }
            @Override public void onError(DnsResolver.DnsException error) {
                if (error.code != DnsResolver.ERROR_SYSTEM
                        || !(error.getCause() instanceof ErrnoException)
                        || ((ErrnoException) error.getCause()).errno != OsConstants.EAGAIN) {
                    failure.compareAndSet(null, error);
                }
                errors.incrementAndGet();
            }
        };
        List<CancellationSignal> signals = new ArrayList<>();
        for (int i = 0; i < 256; i++) {
            CancellationSignal signal = new CancellationSignal();
            signals.add(signal);
            resolver.query(null, "localhost", DnsResolver.TYPE_A, 0, held, signal, callback);
        }
        long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(35);
        while (retained.size() < 256 && System.nanoTime() - until < 0) Thread.sleep(5);
        require(retained.size() == 256, "all 256 real lookup completions reach supplied executor");
        require(ownedRequests() == 256, "retained callbacks consume all real admission slots");
        require(answers.get() == 0 && errors.get() == 0, "held executor owns actual delivery");

        resolver.query(null, "localhost", DnsResolver.TYPE_A, 0, Runnable::run, null, callback);
        require(errors.get() == 1 && failure.get() == null, "257th outstanding query reports EAGAIN");
        for (CancellationSignal signal : signals) signal.cancel();
        for (int i = 0; i < 16; i++) {
            resolver.query(null, "localhost", DnsResolver.TYPE_A, 0, Runnable::run, null, callback);
        }
        require(errors.get() == 17 && failure.get() == null,
                "cancellation does not recycle executor-retained accepted wrappers");
        int lookupThreads = 0, callbackThreads = 0, deadlineThreads = 0;
        for (Thread thread : Thread.getAllStackTraces().keySet()) {
            if (thread.getName().startsWith("DnsResolver-lookup-")) lookupThreads++;
            if (thread.getName().startsWith("DnsResolver-callback-")) callbackThreads++;
            if (thread.getName().startsWith("DnsResolver-deadline-")) deadlineThreads++;
        }
        require(lookupThreads <= 32 && callbackThreads <= 4 && deadlineThreads <= 1,
                "actual ART resolver workers stay within 32/4/1");
        Runnable task;
        while ((task = retained.poll()) != null) task.run();
        require(answers.get() == 0 && errors.get() == 17, "cancelled held callbacks remain silent");
        drained();
        localhost(resolver);
        System.out.println("DnsAdmissionTest passed: " + checks
                + " real Windows lookup, ART admission, retained callback cancellation and recovery checks");
    }
}
