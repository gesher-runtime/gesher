// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.io.*;
import java.lang.reflect.*;
import java.net.*;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Shared host/ART read contracts over the actual HTTP provider and loopback wire. */
public final class HttpEngineReadBufferTest {
    private static final AtomicInteger checks = new AtomicInteger();
    private static boolean baseline;
    private static void check(boolean condition, String message) {
        checks.incrementAndGet(); if (!condition) throw new AssertionError(message);
    }
    private static HttpEngine engine() {
        return new HttpEngine.Builder(new android.content.ContextWrapper(null)).build();
    }
    private static int value(long index) { return (int) ((index * 31 + 17) & 255); }
    private static final class Peer implements AutoCloseable {
        final ServerSocket listener = new ServerSocket(0, 16, InetAddress.getByName("127.0.0.1"));
        final ExecutorService workers = Executors.newCachedThreadPool();
        final ConcurrentLinkedQueue<Throwable> failures = new ConcurrentLinkedQueue<>();
        final Thread accept;
        volatile boolean closed;
        Peer() throws IOException {
            accept = new Thread(() -> {
                try { while (!closed) { Socket s = listener.accept(); workers.execute(() -> serve(s)); } }
                catch (Throwable error) { if (!closed) failures.add(error); }
            }, "request-buffer-peer");
            accept.setDaemon(true); accept.start();
        }
        String url(String mode, int size, int chunk) {
            return "http://127.0.0.1:" + listener.getLocalPort() + "/" + mode + "/" + size + "/" + chunk;
        }
        void serve(Socket socket) {
            String mode = "";
            try (Socket s = socket) {
                s.setSoTimeout(10000);
                BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), "US-ASCII"));
                String request = in.readLine(); if (request == null) return;
                String path = request.split(" ")[1]; String[] parts = path.split("/");
                mode = parts[1]; int size = Integer.parseInt(parts[2]), chunk = Integer.parseInt(parts[3]);
                for (String line; (line = in.readLine()) != null && !line.isEmpty();) {}
                OutputStream out = s.getOutputStream();
                if (mode.equals("redirect")) {
                    out.write(("HTTP/1.1 302 Found\r\nLocation: /bytes/" + size + "/" + chunk
                            + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").getBytes("US-ASCII"));
                    return;
                }
                out.write(("HTTP/1.1 200 OK\r\nContent-Length: " + (mode.equals("truncated") ? size + 1 : size)
                        + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n").getBytes("US-ASCII"));
                byte[] bytes = new byte[chunk];
                for (int offset = 0; offset < size;) {
                    int count = Math.min(chunk, size - offset);
                    for (int i = 0; i < count; i++) bytes[i] = (byte) value((long) offset + i);
                    out.write(bytes, 0, count); offset += count;
                }
            } catch (Throwable error) {
                if (!closed && !mode.equals("cancel")) failures.add(error);
            }
        }
        public void close() throws Exception {
            closed = true; listener.close(); accept.join(5000); workers.shutdownNow();
            check(workers.awaitTermination(10, TimeUnit.SECONDS), "Peer workers did not retire");
            check(!accept.isAlive() && failures.isEmpty(), "Independent peer failed: " + failures);
        }
    }

    /** Test-only observation at the provider stream boundary; bytes still come from the real socket. */
    private static final class Reads extends FilterInputStream {
        byte[] previous;
        long arrays, arrayBytes, calls, bytes;
        int expected, shortLimit = Integer.MAX_VALUE, maxCapacity;
        final CountDownLatch entered, release;
        Reads(InputStream in, boolean gate) {
            super(in); entered = gate ? new CountDownLatch(1) : null;
            release = gate ? new CountDownLatch(1) : null;
        }
        @Override public int read(byte[] target, int offset, int length) throws IOException {
            check(offset == 0 && length == Math.min(32768, expected), "Read ignored the caller's remaining range");
            check(target.length >= length && target.length <= 32768, "Unbounded or undersized read storage");
            if (previous != target) { arrays++; arrayBytes += target.length; previous = target; }
            maxCapacity = Math.max(maxCapacity, target.length); calls++;
            int n = in.read(target, offset, Math.min(length, shortLimit));
            if (n > 0) bytes += n;
            if (entered != null) {
                entered.countDown();
                try { if (!release.await(10, TimeUnit.SECONDS)) throw new IOException("Read ownership gate timed out"); }
                catch (InterruptedException error) { Thread.currentThread().interrupt(); throw new IOException(error); }
            }
            return n;
        }
    }
    private static void retiredArrays(UrlRequest request) throws Exception {
        // Retain the public request while checking internal owned payload retirement.
        // This is a resource assertion, separate from the public byte/callback oracle.
        for (Field f : request.getClass().getDeclaredFields()) if (f.getType() == byte[].class) {
            f.setAccessible(true); check(f.get(request) == null, "Terminal request retained a read payload array");
        }
    }
    private static final class Transfer implements UrlRequest.Callback {
        final HttpEngine engine;
        final CompletableFuture<Transfer> done = new CompletableFuture<>();
        final CompletableFuture<Reads> observed = new CompletableFuture<>();
        final ByteBuffer buffer = ByteBuffer.allocateDirect(70000);
        final int[] capacities;
        final int size, shortLimit;
        final boolean gate, truncated, cancelAtStart;
        final AtomicInteger terminals = new AtomicInteger();
        UrlRequest request;
        Reads reads;
        long received, checksum;
        int count, offset, limit, redirects;
        boolean canceled, failed;
        boolean failReadCallback;
        Transfer(HttpEngine engine, int size, int[] capacities, int shortLimit,
                boolean gate, boolean truncated, boolean cancelAtStart) {
            this.engine = engine; this.size = size; this.capacities = capacities; this.shortLimit = shortLimit;
            this.gate = gate; this.truncated = truncated; this.cancelAtStart = cancelAtStart;
        }
        void action(Runnable action) {
            try { action.run(); }
            catch (Throwable error) { done.completeExceptionally(error); request.cancel(); }
        }
        void next() {
            buffer.clear(); offset = 3 + count % 7;
            int capacity = capacities[count % capacities.length]; limit = offset + capacity;
            buffer.put(offset - 1, (byte) 0x5a); buffer.put(limit, (byte) 0x6b);
            buffer.position(offset); buffer.limit(limit); reads.expected = capacity;
            request.read(buffer);
        }
        @Override public void onRedirectReceived(UrlRequest r, UrlResponseInfo info, String location) {
            action(() -> { redirects++; r.followRedirect(); });
        }
        @Override public void onResponseStarted(UrlRequest r, UrlResponseInfo info) {
            action(() -> {
                try {
                    check(info.getHttpStatusCode() == 200, "Response status"); retiredArrays(r);
                    Field input = r.getClass().getDeclaredField("input"); input.setAccessible(true);
                    reads = new Reads((InputStream) input.get(r), gate); reads.shortLimit = shortLimit;
                    input.set(r, reads); observed.complete(reads);
                    if (cancelAtStart) r.cancel(); else next();
                } catch (Exception error) { throw new RuntimeException(error); }
            });
        }
        @Override public void onReadCompleted(UrlRequest r, UrlResponseInfo info, ByteBuffer delivered) {
            if (failReadCallback) throw new IllegalStateException("Deliberate read callback failure");
            action(() -> {
                check(delivered == buffer && buffer.limit() == limit, "Read changed the caller's buffer or limit");
                int end = buffer.position();
                check(end > offset && end <= limit, "Read position did not advance within the range");
                check(buffer.get(offset - 1) == (byte) 0x5a, "Read overwrote the prefix");
                // Absolute access is also checked against the current limit.
                buffer.limit(buffer.capacity());
                check(buffer.get(limit) == (byte) 0x6b, "Read overwrote the suffix");
                for (int i = offset; i < end; i++) {
                    int actual = buffer.get(i) & 255;
                    if (actual != value(received)) throw new AssertionError("Body mismatch at " + received);
                    checksum += actual; received++;
                }
                count++; next();
            });
        }
        private void terminal(UrlRequest r, UrlResponseInfo info, HttpException error, boolean wasCanceled) {
            try {
                check(terminals.incrementAndGet() == 1, "Multiple terminal callbacks");
                canceled = wasCanceled; failed = error != null;
                check(r.isDone(), "Terminal callback preceded request completion"); retiredArrays(r);
                if (failReadCallback) check(error instanceof CallbackException && !wasCanceled,
                        "Read callback exception lost its terminal failure");
                else if (!wasCanceled && !truncated) {
                    if (error != null) throw error;
                    check(received == size && reads.bytes == size, "Response was incomplete");
                    check(info.getReceivedByteCount() == size, "Uncached network byte count changed");
                }
                if (truncated) check(error != null && !wasCanceled, "Truncated body silently succeeded");
                done.complete(this);
            } catch (Throwable failure) { done.completeExceptionally(failure); }
        }
        @Override public void onSucceeded(UrlRequest r, UrlResponseInfo i) { terminal(r, i, null, false); }
        @Override public void onFailed(UrlRequest r, UrlResponseInfo i, HttpException e) { terminal(r, i, e, false); }
        @Override public void onCanceled(UrlRequest r, UrlResponseInfo i) { terminal(r, i, null, true); }
        Transfer start(String url, Executor callbacks, boolean direct) {
            request = engine.newUrlRequestBuilder(url, callbacks, this).setDirectExecutorAllowed(direct).build();
            request.start(); return this;
        }
        Transfer await() throws Exception {
            try { return done.get(30, TimeUnit.SECONDS); }
            catch (Exception error) { request.cancel(); throw error; }
        }
    }
    private static Transfer transfer(HttpEngine engine, Peer peer, Executor cb, String mode,
            int size, int[] capacities, int shortLimit, boolean direct) throws Exception {
        return new Transfer(engine, size, capacities, shortLimit, false, mode.equals("truncated"), false)
                .start(peer.url(mode, size, 16384), cb, direct).await();
    }
    private static void work(Transfer result, long allocated, long cpuNanos, long nanos, String name) {
        Reads r = result.reads;
        System.out.println("HTTP_REQUEST_READ {\"baseline\":" + baseline + ",\"case\":\"" + name
                + "\",\"bytes\":" + result.received + ",\"callbacks\":" + result.count
                + ",\"read_calls\":" + r.calls + ",\"array_transitions\":" + r.arrays
                + ",\"array_bytes\":" + r.arrayBytes + ",\"max_capacity\":" + r.maxCapacity
                + ",\"worker_allocated_bytes\":" + allocated + ",\"worker_cpu_ns\":" + cpuNanos + ",\"elapsed_ns\":" + nanos
                + ",\"checksum\":" + result.checksum + "}");
        if (!baseline) check(r.arrayBytes <= 65536, "Read scratch allocation grew with body/read count");
        r.previous = null; // The observer must not retain the last payload after reporting.
    }
    /** Optional HotSpot allocation counter; reflection keeps this shared ART fixture portable. */
    private static Map<Long, Long> counters(boolean host, boolean cpu) throws Exception {
        Map<Long, Long> result = new HashMap<>(); if (!host) return result;
        Object bean = Class.forName("java.lang.management.ManagementFactory").getMethod("getThreadMXBean").invoke(null);
        Class<?> common = Class.forName("java.lang.management.ThreadMXBean");
        Class<?> extended = Class.forName("com.sun.management.ThreadMXBean");
        check((Boolean) extended.getMethod("isThreadAllocatedMemorySupported").invoke(bean), "Missing host allocation counter");
        extended.getMethod("setThreadAllocatedMemoryEnabled", boolean.class).invoke(bean, true);
        check((Boolean) common.getMethod("isThreadCpuTimeSupported").invoke(bean), "Missing host CPU counter");
        common.getMethod("setThreadCpuTimeEnabled", boolean.class).invoke(bean, true);
        for (long id : (long[]) common.getMethod("getAllThreadIds").invoke(bean)) {
            Object info = common.getMethod("getThreadInfo", long.class).invoke(bean, id);
            String name = info == null ? "" : (String) Class.forName("java.lang.management.ThreadInfo")
                    .getMethod("getThreadName").invoke(info);
            if (name.equals("Gesher-HttpEngine") || name.equals("Gesher-HttpEngine-unary")
                    || name.equals("Gesher-HttpEngine-retire") || name.equals("Gesher-HttpEngine-complete")) {
                result.put(id, (Long) (cpu ? common.getMethod("getThreadCpuTime", long.class)
                        : extended.getMethod("getThreadAllocatedBytes", long.class)).invoke(bean, id));
            }
        }
        check(!result.isEmpty(), "Host measurement did not observe an HTTP worker");
        return result;
    }
    private static long delta(Map<Long, Long> before, Map<Long, Long> after, boolean host) {
        if (!host || after.isEmpty() || !after.keySet().containsAll(before.keySet())) return -1;
        long value = 0;
        for (Map.Entry<Long, Long> e : after.entrySet()) {
            long start = before.getOrDefault(e.getKey(), 0L), end = e.getValue();
            if (start < 0 || end < start) return -1; // Unavailable/retired counters are not zero work.
            value += end - start;
        }
        return value;
    }
    public static void main(String[] args) throws Exception {
        boolean host = Arrays.asList(args).contains("--host"); baseline = Arrays.asList(args).contains("--baseline");
        if (host) System.setProperty("java.protocol.handler.pkgs", "gesher.http.policy");
        ExecutorService callbacks = Executors.newSingleThreadExecutor();
        try (Peer peer = new Peer()) {
            HttpEngine engine = engine();
            try {
                transfer(engine, peer, callbacks, "bytes", 1024 * 1024, new int[]{32768}, Integer.MAX_VALUE, false);
                Map<Long, Long> before = counters(host, false), cpuBefore = counters(host, true); long start = System.nanoTime();
                // Track94 runs ART's interpreter. Its resource gate needs enough
                // reads to reject per-read allocation, not a host-sized benchmark.
                int largeBytes = (host ? 64 : 2) * 1024 * 1024;
                Transfer large = transfer(engine, peer, callbacks, "bytes", largeBytes,
                        new int[]{32768}, Integer.MAX_VALUE, false);
                long nanos = System.nanoTime() - start; Map<Long, Long> after = counters(host, false), cpuAfter = counters(host, true);
                work(large, delta(before, after, host), delta(cpuBefore, cpuAfter, host), nanos,
                        host ? "64MiB" : "2MiB-ART-workload");
                for (boolean direct : new boolean[]{false, true}) {
                    Transfer mixed = transfer(engine, peer, direct ? Runnable::run : callbacks, "redirect", 256 * 1024,
                            new int[]{1, 7, 257, 8192, 32768, 65521, 9}, 113, direct);
                    check(mixed.redirects == 1, "Redirect chain changed");
                    work(mixed, -1, -1, 0, direct ? "mixed-direct-short" : "mixed-async-short");
                }
                int[] gradual = new int[1024]; for (int i = 0; i < gradual.length; i++) gradual[i] = i + 1;
                Transfer growing = transfer(engine, peer, callbacks, "bytes", 1024 * 1024,
                        gradual, Integer.MAX_VALUE, false);
                work(growing, -1, -1, 0, "gradually-growing");
                List<Transfer> concurrent = new ArrayList<>();
                for (int i = 0; i < 4; i++) concurrent.add(new Transfer(engine, 1024 * 1024,
                        new int[]{13, 32768, 5}, 8191, false, false, false)
                        .start(peer.url("bytes", 1024 * 1024, 4096), callbacks, false));
                for (int i = 0; i < concurrent.size(); i++) {
                    Transfer result = concurrent.get(i).await();
                    for (int j = 0; j < i; j++) check(result.reads.previous != concurrent.get(j).reads.previous,
                            "Concurrent requests shared mutable read storage");
                }
                for (int i = 0; i < concurrent.size(); i++) work(concurrent.get(i), -1, -1, 0, "concurrent-" + i);
                transfer(engine, peer, callbacks, "bytes", 0, new int[]{3}, Integer.MAX_VALUE, false);
                transfer(engine, peer, callbacks, "truncated", 4096, new int[]{1, 32768, 7}, Integer.MAX_VALUE, false);
                Transfer callbackFailure = new Transfer(engine, 4096, new int[]{32768}, Integer.MAX_VALUE,
                        false, false, false);
                callbackFailure.failReadCallback = true;
                callbackFailure.start(peer.url("cancel", 4096, 257), callbacks, false).await();
                Transfer unopened = new Transfer(engine, 4096, new int[]{32768}, Integer.MAX_VALUE, false, false, true)
                        .start(peer.url("cancel", 4096, 257), callbacks, false).await();
                check(unopened.canceled && unopened.reads.arrays == 0, "Cancellation before read allocated storage");
                Transfer blocked = new Transfer(engine, 1024, new int[]{32768}, Integer.MAX_VALUE, true, false, false)
                        .start(peer.url("cancel", 1024, 257), callbacks, false);
                Reads held = blocked.observed.get(5, TimeUnit.SECONDS);
                check(held.entered.await(5, TimeUnit.SECONDS), "Read did not enter the ownership gate");
                blocked.request.cancel();
                boolean active = false; try { engine.shutdown(); } catch (IllegalStateException expected) { active = true; }
                check(active && blocked.terminals.get() == 0, "Cancellation retired an admitted reader early");
                held.release.countDown(); blocked.await();
                check(blocked.canceled && blocked.received == 0 && blocked.buffer.position() == blocked.offset,
                        "Canceled read modified the application buffer or delivered a callback");
                held.previous = null;
                engine.shutdown();
            } finally { engine.shutdown(); }
        } finally { callbacks.shutdownNow(); check(callbacks.awaitTermination(10, TimeUnit.SECONDS), "Callback executor leaked"); }
        System.out.println("HttpEngineReadBufferTest passed: " + checks.get() + " real-provider read/work/ownership checks");
    }
}
