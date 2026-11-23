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
import java.lang.ref.WeakReference;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.RunnableScheduledFuture;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The Connectivity module's DnsResolver (framework.jar compiles against the module's API
 * and does not contain it), with its public API. Apps that name the class (Waze's network
 * stack, when an address is chosen) died with NoClassDefFoundError.
 *
 * <p>Lookups have bounded request, input, result and worker ownership, and resolve
 * A and AAAA records through the
 * network's Windows interface, as {@link Network#getAllByName} does (the process's
 * network, or Windows' default, for a null network); its callback runs on the executor
 * unless the lookup was cancelled. A name that does not resolve is answered with no
 * addresses and NXDOMAIN, as the module does. rawQuery answers A and AAAA questions of
 * class IN with a DNS response built from those addresses (TTL 0); Windows' resolver
 * gives no records of other types, so they are answered NOTIMP.
 */
public final class DnsResolver {
    public static final int CLASS_IN = 1;
    public static final int TYPE_A = 1;
    public static final int TYPE_AAAA = 28;

    public static final int FLAG_EMPTY = 0;
    public static final int FLAG_NO_RETRY = 1 << 0;
    public static final int FLAG_NO_CACHE_STORE = 1 << 1;
    public static final int FLAG_NO_CACHE_LOOKUP = 1 << 2;

    public static final int ERROR_PARSE = 0;
    public static final int ERROR_SYSTEM = 1;

    private static final int RCODE_NOERROR = 0;
    private static final int RCODE_FORMERR = 1;
    private static final int RCODE_NXDOMAIN = 3;
    private static final int RCODE_NOTIMP = 4;

    private static final DnsResolver sInstance = new DnsResolver();

    public static DnsResolver getInstance() {
        return sInstance;
    }

    private DnsResolver() {
    }

    public interface Callback<T> {
        void onAnswer(T answer, int rcode);

        void onError(DnsException error);
    }

    public static class DnsException extends Exception {
        public final int code;

        public DnsException(int code, Throwable cause) {
            super(cause);
            this.code = code;
        }
    }

    public void rawQuery(Network network, byte[] query, int flags, Executor executor,
            CancellationSignal cancellationSignal, Callback<? super byte[]> callback) {
        check(executor, callback);
        if (query == null) throw new NullPointerException("query");
        submit(cancellationSignal, executor, callback, query.length, () -> {
            byte[] copy = query.clone(); // Admission owns these bytes before copying.
            return () -> answerRaw(network, copy);
        });
    }

    public void rawQuery(Network network, String domain, int nsClass, int nsType, int flags,
            Executor executor, CancellationSignal cancellationSignal,
            Callback<? super byte[]> callback) {
        check(executor, callback);
        if (domain == null) throw new NullPointerException("domain");
        submit(cancellationSignal, executor, callback, 2L * domain.length(), () -> () -> {
            byte[] query;
            try {
                query = question(domain, nsClass, nsType);
            } catch (IllegalArgumentException e) {
                throw new DnsException(ERROR_PARSE, e);
            }
            return answerRaw(network, query);
        });
    }

    public void query(Network network, String domain, int flags, Executor executor,
            CancellationSignal cancellationSignal, Callback<? super List<InetAddress>> callback) {
        query(network, domain, -1, flags, executor, cancellationSignal, callback);
    }

    public void query(Network network, String domain, int nsType, int flags, Executor executor,
            CancellationSignal cancellationSignal, Callback<? super List<InetAddress>> callback) {
        check(executor, callback);
        if (domain == null) throw new NullPointerException("domain");
        submit(cancellationSignal, executor, callback, 2L * domain.length(), () -> () -> {
            List<InetAddress> addresses = new ArrayList<>();
            int rcode = lookup(network, domain, nsType, addresses);
            return new Answer<>(addresses, rcode);
        });
    }

    private static void check(Executor executor, Callback<?> callback) {
        if (executor == null) throw new NullPointerException("executor");
        if (callback == null) throw new NullPointerException("callback");
    }

    private interface Lookup<T> {
        Answer<T> call() throws DnsException;
    }

    private static final class Answer<T> {
        final T value;
        final int rcode;
        final long retainedBytes;

        Answer(T value, int rcode) {
            this.value = value;
            this.rcode = rcode;
            retainedBytes = value instanceof byte[] ? ((byte[]) value).length + 64L
                    : 128L * ((List<?>) value).size() + 256L;
        }
    }

    private interface LookupFactory<T> {
        Lookup<T> create();
    }

    // These are resolver-owned budgets, not a limit on application Executor queues.
    // In particular, AOSP reports failure to admit a request through the supplied
    // Executor on the API caller; those rejected requests never enter these owners.
    private static final int MAX_OUTSTANDING = 256;
    private static final int MAX_LOOKUPS = 32;
    private static final int MAX_DISPATCHERS = 4;
    private static final int MAX_INPUT_BYTES = 65_535;
    private static final int MAX_RESPONSE_BYTES = 65_535;
    private static final int MAX_ADDRESSES = 4096;
    private static final long INPUT_BUDGET = 8L * 1024 * 1024;
    private static final long ANSWER_BUDGET = 8L * 1024 * 1024;
    private static final long DEADLINE_NANOS = TimeUnit.SECONDS.toNanos(30);
    private static final long COMPLETION_RETRY_MILLIS = 1000;
    private static final Object sLock = new Object();
    private static final LinkedHashSet<Request<?>> sRequests = new LinkedHashSet<>();
    private static long sInputBytes, sAnswerBytes;
    private static final ThreadPoolExecutor sLookups = pool(MAX_LOOKUPS, "lookup");
    private static final ThreadPoolExecutor sDispatchers = pool(MAX_DISPATCHERS, "callback");
    private static final DeadlineExecutor sDeadlines = deadlines();

    private static ThreadFactory threads(String role) {
        AtomicInteger ids = new AtomicInteger();
        return runnable -> {
            Thread thread = new Thread(runnable, "DnsResolver-" + role + "-" + ids.incrementAndGet());
            thread.setDaemon(true);
            return thread;
        };
    }

    private static ThreadPoolExecutor pool(int workers, String name) {
        ThreadPoolExecutor pool = new ThreadPoolExecutor(workers, workers, 30, TimeUnit.SECONDS,
                new ArrayBlockingQueue<>(MAX_OUTSTANDING), threads(name));
        pool.allowCoreThreadTimeOut(true);
        return pool;
    }

    private static DeadlineExecutor deadlines() {
        DeadlineExecutor pool = new DeadlineExecutor();
        pool.setRemoveOnCancelPolicy(true);
        pool.setKeepAliveTime(30, TimeUnit.SECONDS);
        pool.allowCoreThreadTimeOut(true);
        return pool;
    }

    private static final class Timer implements Runnable {
        final Runnable action;
        RunnableScheduledFuture<?> future;
        Timer(Runnable action) { this.action = action; }
        @Override public void run() { action.run(); }
    }

    private static final class DeadlineExecutor extends ScheduledThreadPoolExecutor {
        DeadlineExecutor() { super(1, threads("deadline")); }
        @Override protected <V> RunnableScheduledFuture<V> decorateTask(
                Runnable action, RunnableScheduledFuture<V> task) {
            // schedule() queues its future before starting the worker. Capture the
            // actual future before that insertion so a Thread.start failure cannot
            // leave an unreachable timer after admission is rolled back.
            if (action instanceof Timer) ((Timer) action).future = task;
            return task;
        }
    }

    private static ScheduledFuture<?> schedule(Runnable action, long delay, TimeUnit unit) {
        Timer timer = new Timer(action);
        try {
            return sDeadlines.schedule(timer, delay, unit);
        } catch (RuntimeException | Error unavailable) {
            if (timer.future != null) {
                timer.future.cancel(false);
                sDeadlines.remove(timer.future);
            }
            throw unavailable;
        }
    }

    private static DnsException systemError(String operation, int errno) {
        return new DnsException(ERROR_SYSTEM, new ErrnoException(operation, errno));
    }

    private static <T> void rejected(CancellationSignal signal, Executor executor,
            Callback<? super T> callback, DnsException error) {
        // Match AOSP's immediate resNetwork* failure path. No lookup, copied input,
        // answer or internal queue is captured here. The caller owns this Executor;
        // it may execute inline, block, reject or retain its error notification.
        executor.execute(() -> {
            if (signal == null || !signal.isCanceled()) callback.onError(error);
        });
    }

    private static <T> void submit(CancellationSignal signal, Executor executor,
            Callback<? super T> callback, long inputBytes, LookupFactory<T> factory) {
        if (signal != null && signal.isCanceled()) return;
        if (inputBytes > MAX_INPUT_BYTES) {
            rejected(signal, executor, callback, systemError("resNetworkQuery", OsConstants.EMSGSIZE));
            return;
        }
        Request<T> request;
        synchronized (sLock) {
            if (sRequests.size() == MAX_OUTSTANDING || inputBytes > INPUT_BUDGET - sInputBytes) {
                request = null;
            } else {
                request = new Request<>(signal, executor, callback, inputBytes);
                sRequests.add(request);
                sInputBytes += inputBytes;
            }
        }
        if (request == null) {
            rejected(signal, executor, callback, systemError("resNetworkQuery", OsConstants.EAGAIN));
            return;
        }
        boolean submitted = false;
        try {
            request.prepare(factory.create());
            if (signal != null) {
                // No listener detachment: completing an older request must not erase
                // a newer registration on the same CancellationSignal. The listener
                // owns no request or payload, and cancels every current use of it.
                signal.setOnCancelListener(new Cancellation(signal));
            }
            synchronized (sLock) {
                request.preparing = false;
                if (request.cancelled || signal != null && signal.isCanceled()) {
                    request.cancelLocked();
                    return;
                }
                request.lookupOwned = true;
                request.deadline = schedule(request::timeout,
                        Math.max(0, request.deadlineNanos - System.nanoTime()), TimeUnit.NANOSECONDS);
                // The internal executor never runs work inline. Serializing enqueue
                // with cancellation makes a queued task removable before lookup.
                sLookups.execute(request);
                submitted = true;
            }
        } catch (RuntimeException failure) {
            synchronized (sLock) {
                request.preparing = false;
                if (!submitted) request.lookupOwned = false;
                request.cancelLocked();
            }
            rejected(signal, executor, callback, new DnsException(ERROR_SYSTEM, failure));
        } catch (Error failure) {
            synchronized (sLock) {
                request.preparing = false;
                if (!submitted) request.lookupOwned = false;
                request.cancelLocked();
            }
            throw failure;
        }
    }

    private static final class Cancellation implements CancellationSignal.OnCancelListener {
        final WeakReference<CancellationSignal> signal;
        Cancellation(CancellationSignal signal) { this.signal = new WeakReference<>(signal); }
        @Override public void onCancel() {
            CancellationSignal cancelled = signal.get();
            if (cancelled == null) return;
            synchronized (sLock) {
                // Retirement changes sRequests, so iteration needs a bounded snapshot.
                for (Request<?> request : new ArrayList<>(sRequests)) {
                    if (request.signal == cancelled) request.cancelLocked();
                }
            }
        }
    }

    /** One admission remains owned until every external operation actually returns. */
    private static final class Request<T> implements Runnable {
        final CancellationSignal signal;
        final long inputBytes;
        final long deadlineNanos = System.nanoTime() + DEADLINE_NANOS;
        final Runnable dispatchTask = this::dispatch;
        final Runnable deliveryTask = this::deliver;
        Executor executor;
        Callback<? super T> callback;
        Lookup<T> lookup;
        Answer<T> answer;
        DnsException error;
        ScheduledFuture<?> deadline;
        ScheduledFuture<?> completionRetry;
        long answerBytes;
        boolean preparing = true, lookupOwned, dispatchOwned, callbackOwned;
        boolean terminal, cancelled, callbackStarted, retired, retryPending;

        Request(CancellationSignal signal, Executor executor, Callback<? super T> callback,
                long inputBytes) {
            this.signal = signal; this.executor = executor; this.callback = callback;
            this.inputBytes = inputBytes;
        }

        void prepare(Lookup<T> prepared) {
            synchronized (sLock) { if (!cancelled) lookup = prepared; }
        }

        @Override public void run() {
            Lookup<T> operation;
            synchronized (sLock) {
                if (signal != null && signal.isCanceled()) cancelLocked();
                // Timer dispatch can be delayed. An expired queued request must
                // not enter an uncancelable native lookup merely because its
                // scheduled timeout has not acquired the owner lock yet.
                if (!terminal && System.nanoTime() - deadlineNanos >= 0) timeoutLocked();
                if (cancelled || terminal) {
                    lookup = null;
                    lookupOwned = false;
                    retireLocked();
                    return;
                }
                operation = lookup;
                lookup = null;
            }
            Answer<T> result = null;
            DnsException failure = null;
            try {
                result = operation.call();
                if (result == null) throw new IllegalStateException("Missing DNS result");
            } catch (DnsException e) {
                failure = e;
            } catch (Throwable e) {
                failure = new DnsException(ERROR_SYSTEM, e);
            }
            operation = null;
            synchronized (sLock) {
                // Up to MAX_LOOKUPS producers can own one bounded result while
                // waiting for retained-answer credit. Completion uses another lane.
                while (!terminal && failure == null
                        && result.retainedBytes > ANSWER_BUDGET - sAnswerBytes) {
                    long remaining = deadlineNanos - System.nanoTime();
                    if (remaining <= 0) { timeoutLocked(); break; }
                    try { TimeUnit.NANOSECONDS.timedWait(sLock, remaining); }
                    catch (InterruptedException interrupted) {
                        failure = systemError("resNetworkResult", OsConstants.EINTR);
                    }
                }
                if (!terminal && System.nanoTime() - deadlineNanos >= 0) timeoutLocked();
                if (signal != null && signal.isCanceled()) cancelLocked();
                if (!terminal) {
                    terminal = true;
                    answer = failure == null ? result : null;
                    error = failure;
                    if (failure == null) {
                        answerBytes = result.retainedBytes;
                        sAnswerBytes += answerBytes;
                    }
                    enqueueCompletionLocked();
                }
                result = null;
                failure = null;
                // A timeout or cancellation never releases a still-running native
                // lookup. Only reaching this point retires its actual ownership.
                lookupOwned = false;
                retireLocked();
            }
        }

        void timeout() {
            synchronized (sLock) { timeoutLocked(); }
        }

        void timeoutLocked() {
            if (terminal || retired) return;
            if (signal != null && signal.isCanceled()) { cancelLocked(); return; }
            terminal = true;
            lookup = null;
            if (sLookups.remove(this)) lookupOwned = false;
            error = systemError("resNetworkResult", OsConstants.ETIMEDOUT);
            enqueueCompletionLocked();
            sLock.notifyAll();
        }

        void cancelLocked() {
            if (retired) return;
            cancelled = true;
            terminal = true;
            cancelDeadlineLocked();
            lookup = null;
            if (sLookups.remove(this)) lookupOwned = false;
            if (sDispatchers.remove(dispatchTask)) dispatchOwned = false;
            if (retryPending) {
                retryPending = false;
                if (completionRetry != null) completionRetry.cancel(false);
                completionRetry = null;
            }
            clearPayloadLocked();
            retireLocked();
            sLock.notifyAll();
        }

        void cancelDeadlineLocked() {
            if (deadline != null) { deadline.cancel(false); deadline = null; }
        }

        void enqueueCompletionLocked() {
            dispatchOwned = true;
            // At most one completion per admitted request. Queue capacity is at
            // least the entire admission bound, and this executor is never shut down.
            sDispatchers.getQueue().add(dispatchTask);
            startCompletionWorkerLocked();
            // Until a dispatcher exists or its retry is scheduled, retain the
            // deadline. Its queued/running task keeps the one timer worker alive
            // even if this thread is descheduled beyond the idle timeout. After
            // ownership transfers, all internal pools can still retire when idle.
            cancelDeadlineLocked();
        }

        void startCompletionWorkerLocked() {
            try {
                // Queue exactly once before starting a worker. Existing workers
                // can consume this task even when starting another worker fails;
                // retries never enqueue duplicate completion wrappers.
                sDispatchers.prestartCoreThread();
            } catch (RuntimeException | Error unavailable) {
                // A real worker thread can fail to start even with queue capacity
                // available. Keep this same completion owner and payload; never
                // run application code under the lock or on a lookup worker.
                // This request's deadline is still queued/running, or this is
                // the timer worker itself retrying. No new timer thread is needed.
                retryPending = true;
                completionRetry = schedule(this::retryCompletion,
                        COMPLETION_RETRY_MILLIS, TimeUnit.MILLISECONDS);
            }
        }

        void retryCompletion() {
            synchronized (sLock) {
                completionRetry = null;
                if (!retryPending || cancelled || retired) return;
                retryPending = false;
                startCompletionWorkerLocked();
            }
        }

        void dispatch() {
            Executor target;
            synchronized (sLock) {
                retryPending = false;
                if (completionRetry != null) completionRetry.cancel(false);
                completionRetry = null;
                if (cancelled || retired) {
                    dispatchOwned = false;
                    retireLocked();
                    return;
                }
                target = executor;
                executor = null;
                callbackOwned = true; // execute() is allowed to run the task inline.
            }
            try {
                target.execute(deliveryTask);
            } catch (RejectedExecutionException rejected) {
                synchronized (sLock) {
                    // Executor's documented rejection means it did not accept the
                    // task. An inline callback may instead have thrown this exception;
                    // its own finally has already retired callback ownership.
                    if (!callbackStarted) {
                        callbackOwned = false;
                        cancelled = true;
                        clearPayloadLocked();
                    }
                }
            } catch (Throwable failure) {
                synchronized (sLock) {
                    // A nonconforming executor can enqueue and then throw. Unless
                    // the callback actually ran, its wrapper remains charged until
                    // it runs. Clear bulky state, but do not recycle that admission.
                    if (!callbackStarted) {
                        cancelled = true;
                        clearPayloadLocked();
                    }
                }
            } finally {
                target = null;
                synchronized (sLock) {
                    dispatchOwned = false;
                    retireLocked();
                }
            }
        }

        void deliver() {
            Callback<? super T> target;
            Answer<T> value;
            DnsException failure;
            synchronized (sLock) {
                if (callbackStarted) return;
                callbackStarted = true;
                if (cancelled || retired || signal != null && signal.isCanceled()) {
                    callbackOwned = false;
                    clearPayloadLocked();
                    retireLocked();
                    return;
                }
                target = callback;
                value = answer;
                failure = error;
                callback = null; answer = null; error = null;
            }
            try {
                if (failure != null) target.onError(failure);
                else target.onAnswer(value.value, value.rcode);
            } finally {
                target = null; value = null; failure = null;
                synchronized (sLock) {
                    callbackOwned = false;
                    releaseAnswerLocked();
                    retireLocked();
                }
            }
        }

        void clearPayloadLocked() {
            executor = null; callback = null; answer = null; error = null;
            // An already running callback holds its answer in a local variable.
            if (!callbackStarted || !callbackOwned) releaseAnswerLocked();
        }

        void releaseAnswerLocked() {
            sAnswerBytes -= answerBytes;
            answerBytes = 0;
            sLock.notifyAll();
        }

        void retireLocked() {
            if (retired || preparing || lookupOwned || dispatchOwned || callbackOwned) return;
            retired = true;
            cancelDeadlineLocked();
            clearPayloadLocked();
            lookup = null;
            sInputBytes -= inputBytes;
            sRequests.remove(this);
            sLock.notifyAll();
        }
    }

    /** Resolves {@code domain}'s addresses of {@code nsType} (-1: both) into {@code out}. */
    private static int lookup(Network network, String domain, int nsType, List<InetAddress> out)
            throws DnsException {
        int netId = network != null ? network.netId : WindowsNetworks.processNetwork();
        InetAddress[] all;
        try {
            all = WindowsNetworks.resolve(netId, domain);
        } catch (UnknownHostException e) {
            return RCODE_NXDOMAIN;
        } catch (IllegalArgumentException e) {
            throw new DnsException(ERROR_SYSTEM, e);
        }
        if (all.length > MAX_ADDRESSES)
            throw systemError("resNetworkResult", OsConstants.ENOBUFS);
        for (InetAddress address : all) {
            if (nsType == -1 || (nsType == TYPE_A && address instanceof Inet4Address)
                    || (nsType == TYPE_AAAA && address instanceof Inet6Address)) {
                out.add(address);
            }
        }
        return RCODE_NOERROR;
    }

    private static Answer<byte[]> answerRaw(Network network, byte[] query) throws DnsException {
        // Header (12 bytes), then one question: name, type, class.
        if (query.length < 12) throw new DnsException(ERROR_PARSE, null);
        int qdcount = u16(query, 4);
        int pos = 12;
        StringBuilder name = new StringBuilder();
        try {
            while (true) {
                int len = query[pos++] & 0xff;
                if (len == 0) break;
                if (len > 63) throw new DnsException(ERROR_PARSE, null);
                if (name.length() > 0) name.append('.');
                name.append(new String(query, pos, len, StandardCharsets.ISO_8859_1));
                pos += len;
            }
            if (pos + 4 > query.length) throw new DnsException(ERROR_PARSE, null);
        } catch (ArrayIndexOutOfBoundsException | StringIndexOutOfBoundsException e) {
            throw new DnsException(ERROR_PARSE, e);
        }
        int qtype = u16(query, pos);
        int qclass = u16(query, pos + 2);
        int questionEnd = pos + 4;

        List<InetAddress> addresses = new ArrayList<>();
        int rcode;
        if (qdcount != 1) {
            rcode = RCODE_FORMERR;
        } else if (qclass != CLASS_IN || (qtype != TYPE_A && qtype != TYPE_AAAA)) {
            rcode = RCODE_NOTIMP;
        } else {
            rcode = lookup(network, name.toString(), qtype, addresses);
        }

        long responseBytes = 12L + (qdcount == 1 ? questionEnd - 12 : 0);
        for (InetAddress address : addresses)
            responseBytes += address instanceof Inet4Address ? 16 : 28;
        if (responseBytes > MAX_RESPONSE_BYTES)
            throw systemError("resNetworkResult", OsConstants.EMSGSIZE);
        ByteArrayOutputStream out = new ByteArrayOutputStream((int) responseBytes);
        out.write(query[0]);  // id
        out.write(query[1]);
        // QR, the query's opcode and RD, RA; then the rcode.
        out.write(0x80 | (query[2] & 0x79));
        out.write(0x80 | rcode);
        put16(out, qdcount == 1 ? 1 : 0);
        put16(out, addresses.size());
        put16(out, 0);
        put16(out, 0);
        if (qdcount == 1) out.write(query, 12, questionEnd - 12);
        for (InetAddress address : addresses) {
            byte[] raw = address.getAddress();
            put16(out, 0xc00c);  // a pointer to the question's name
            put16(out, raw.length == 4 ? TYPE_A : TYPE_AAAA);
            put16(out, CLASS_IN);
            put16(out, 0);  // TTL 0: Windows' resolver does not give it
            put16(out, 0);
            put16(out, raw.length);
            out.write(raw, 0, raw.length);
        }
        return new Answer<>(out.toByteArray(), rcode);
    }

    /** A recursive query for one question, with a random id. */
    private static byte[] question(String domain, int nsClass, int nsType) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        put16(out, (int) (Math.random() * 0x10000));
        put16(out, 0x0100);  // RD
        put16(out, 1);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        for (String label : domain.split("\\.")) {
            if (label.isEmpty()) continue;
            byte[] bytes = label.getBytes(StandardCharsets.ISO_8859_1);
            if (bytes.length > 63) throw new IllegalArgumentException("label too long: " + label);
            out.write(bytes.length);
            out.write(bytes, 0, bytes.length);
        }
        out.write(0);
        put16(out, nsType);
        put16(out, nsClass);
        return out.toByteArray();
    }

    private static int u16(byte[] b, int at) {
        return ((b[at] & 0xff) << 8) | (b[at + 1] & 0xff);
    }

    private static void put16(ByteArrayOutputStream out, int v) {
        out.write((v >> 8) & 0xff);
        out.write(v & 0xff);
    }
}
