// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import android.net.Network;
import java.io.*;
import java.lang.reflect.Field;
import java.net.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.BooleanSupplier;
import javax.net.ssl.*;

/** Engine-owned HTTP/2 sessions. Stream buffers and cancellation remain private. */
final class Http2SessionPool implements Closeable {
    private static final long IDLE_NANOS = TimeUnit.SECONDS.toNanos(60);
    private static final ScheduledThreadPoolExecutor EXPIRY = expiryExecutor();
    private static ScheduledThreadPoolExecutor expiryExecutor() {
        ScheduledThreadPoolExecutor executor = new ScheduledThreadPoolExecutor(1, task -> {
            Thread thread = new Thread(task, "Gesher-Http2-idle");
            thread.setDaemon(true);
            return thread;
        });
        executor.setRemoveOnCancelPolicy(true);
        // One process-wide daemon parks on the delay queue. A core timeout
        // would repeatedly poll while a longer idle deadline remains queued.
        return executor;
    }

    /** Exact route and TLS factory identity; never coalesce different origins. */
    static final class Route {
        final String scheme, host;
        final int port;
        final Network network;
        final Proxy proxy;
        final SSLSocketFactory tlsFactory;
        Route(URL url, Network network) throws Exception {
            scheme = url.getProtocol().toLowerCase(Locale.ROOT);
            host = url.getHost().toLowerCase(Locale.ROOT);
            port = url.getPort() < 0 ? url.getDefaultPort() : url.getPort();
            this.network = network;
            tlsFactory = scheme.equals("https") ? HttpsURLConnection.getDefaultSSLSocketFactory() : null;
            if (tlsFactory == null) {
                Class<?> type = Class.forName("android.security.NetworkSecurityPolicy");
                Object policy = Http2Transport.call(type, null, "getInstance", new Class<?>[0]);
                if (!(Boolean) Http2Transport.call(type, policy, "isCleartextTrafficPermitted",
                        new Class<?>[]{String.class}, host)) {
                    throw new IOException("Cleartext traffic is not permitted");
                }
            }
            ProxySelector selector = ProxySelector.getDefault();
            List<Proxy> selected = selector == null ? null : selector.select(url.toURI());
            proxy = selected == null || selected.isEmpty() ? Proxy.NO_PROXY : selected.get(0);
            if (proxy.type() == Proxy.Type.SOCKS && network != null) {
                throw new IOException("SOCKS with explicit network binding is unsupported");
            }
            if (proxy.type() == Proxy.Type.HTTP && tlsFactory == null) {
                throw new IOException("Cleartext HTTP/2 over HTTP proxy is unsupported");
            }
        }
        @Override public boolean equals(Object other) {
            if (!(other instanceof Route)) return false;
            Route route = (Route) other;
            return port == route.port && network == route.network && tlsFactory == route.tlsFactory
                    && scheme.equals(route.scheme) && host.equals(route.host) && proxy.equals(route.proxy);
        }
        @Override public int hashCode() {
            return Objects.hash(scheme, host, port, System.identityHashCode(network),
                    System.identityHashCode(tlsFactory), proxy);
        }
    }

    static final class Session implements Closeable {
        final Route route;
        final Object starts = new Object();
        private volatile Socket socket;
        volatile Object connection;
        private Field shutdown;
        private volatile boolean closed, ready;
        // The pool monitor owns these fields; each transport holds one lease.
        int leases = 1;
        long idleSince;

        Session(Route route) { this.route = route; }
        private synchronized void retain(Socket value) throws IOException {
            if (closed) { value.close(); throw new IOException("HTTP/2 session closed"); }
            socket = value;
        }
        boolean reusable() throws Exception {
            Object value = connection;
            if (!ready || closed || socket.isClosed()) return false;
            synchronized (value) {
                return !shutdown.getBoolean(value) && leases < streamLimit(value);
            }
        }
        boolean healthy() {
            Object value = connection;
            if (!ready || closed || socket.isClosed()) return false;
            try { synchronized (value) { return !shutdown.getBoolean(value); } }
            catch (IllegalAccessException unavailable) { return false; }
        }
        private static long streamLimit(Object connection) throws Exception {
            // SETTINGS_MAX_CONCURRENT_STREAMS is an unsigned 32-bit value.
            return Integer.toUnsignedLong((Integer) Http2Transport.call(connection.getClass(),
                    connection, "maxConcurrentStreams", new Class<?>[0]));
        }
        void connect() throws Exception {
            if (ready) return;
            try {
                Network network = route.network;
                Proxy proxy = route.proxy;
                Socket raw = network != null ? network.getSocketFactory().createSocket()
                        : new Socket(proxy.type() == Proxy.Type.SOCKS ? proxy : Proxy.NO_PROXY);
                retain(raw);
                raw.setTcpNoDelay(true);
                raw.setSoTimeout(30000);
                InetSocketAddress address = proxy.type() == Proxy.Type.HTTP
                        ? (InetSocketAddress) proxy.address()
                        : proxy.type() == Proxy.Type.SOCKS
                                ? InetSocketAddress.createUnresolved(route.host, route.port)
                                : new InetSocketAddress(network == null ? InetAddress.getByName(route.host)
                                        : network.getByName(route.host), route.port);
                if (proxy.type() == Proxy.Type.HTTP && address.isUnresolved()) {
                    address = new InetSocketAddress(network == null ? InetAddress.getByName(address.getHostString())
                            : network.getByName(address.getHostString()), address.getPort());
                }
                raw.connect(address, 30000);
                if (proxy.type() == Proxy.Type.HTTP) {
                    String host = route.host;
                    if (host.contains(":") && !host.startsWith("[")) host = "[" + host + "]";
                    String authority = host + ":" + route.port;
                    OutputStream out = raw.getOutputStream();
                    out.write(("CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority
                            + "\r\n\r\n").getBytes("US-ASCII"));
                    out.flush();
                    InputStream in = raw.getInputStream();
                    String status = line(in);
                    if (!status.matches("HTTP/1\\.[01] 200(?: .*|)")) {
                        throw new IOException("HTTP proxy refused CONNECT");
                    }
                    int length = status.length();
                    for (String header; !(header = line(in)).isEmpty();) {
                        length += header.length();
                        if (length > 65536) throw new IOException("Proxy response headers too large");
                    }
                }
                if (route.tlsFactory != null) {
                    SSLSocket ssl = (SSLSocket) route.tlsFactory.createSocket(raw, route.host, route.port, true);
                    retain(ssl);
                    SSLParameters parameters = ssl.getSSLParameters();
                    parameters.setEndpointIdentificationAlgorithm("HTTPS");
                    parameters.setApplicationProtocols(new String[]{"h2"});
                    ssl.setSSLParameters(parameters);
                    ssl.startHandshake();
                    if (!"h2".equals(ssl.getApplicationProtocol())) {
                        throw new SSLException("Server did not negotiate HTTP/2");
                    }
                }
                // Shared connections have independent per-stream I/O deadlines.
                // A socket-wide read timeout would let one idle stream kill peers.
                socket.setSoTimeout(0);
                String framed = "com.android.okhttp.internal.framed.FramedConnection";
                Class<?> builderType = Class.forName(framed + "$Builder");
                Class<?> protocol = Class.forName("com.android.okhttp.Protocol");
                Object builder = builderType.getConstructor(boolean.class).newInstance(true);
                Class<?> okio = Class.forName("com.android.okhttp.okio.Okio");
                Class<?> sourceType = Class.forName("com.android.okhttp.okio.Source");
                Class<?> sinkType = Class.forName("com.android.okhttp.okio.Sink");
                Object source = Http2Transport.call(okio, null, "source", new Class<?>[]{Socket.class}, socket);
                Object sink = Http2Transport.call(okio, null, "sink", new Class<?>[]{Socket.class}, socket);
                // Headers and connection-control frames are written outside a
                // FramedStream's timeout. Keep a physical write deadline too:
                // a peer that stops reading must not pin all owners forever.
                Object timeout = Http2Transport.call(sinkType, sink, "timeout", new Class<?>[0]);
                Http2Transport.call(Class.forName("com.android.okhttp.okio.Timeout"), timeout, "timeout",
                        new Class<?>[]{long.class, TimeUnit.class}, 30L, TimeUnit.SECONDS);
                Object bufferedSource = Http2Transport.call(okio, null, "buffer", new Class<?>[]{sourceType}, source);
                Object bufferedSink = Http2Transport.call(okio, null, "buffer", new Class<?>[]{sinkType}, sink);
                Http2Transport.call(builderType, builder, "socket", new Class<?>[]{Socket.class, String.class,
                        Class.forName("com.android.okhttp.okio.BufferedSource"),
                        Class.forName("com.android.okhttp.okio.BufferedSink")},
                        socket, route.host, bufferedSource, bufferedSink);
                Http2Transport.call(builderType, builder, "protocol", new Class<?>[]{protocol},
                        protocol.getField("HTTP_2").get(null));
                Object opened = Http2Transport.call(builderType, builder, "build", new Class<?>[0]);
                synchronized (this) {
                    if (closed) { ((Closeable) opened).close(); throw new IOException("HTTP/2 session closed"); }
                    connection = opened;
                }
                shutdown = opened.getClass().getDeclaredField("shutdown");
                shutdown.setAccessible(true);
                Http2Transport.call(opened.getClass(), opened, "sendConnectionPreface", new Class<?>[0]);
                ready = true;
            } catch (Exception | Error failure) {
                try { close(); } catch (IOException cleanup) { failure.addSuppressed(cleanup); }
                throw failure;
            }
        }
        private static String line(InputStream in) throws IOException {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            for (int ch; (ch = in.read()) != -1;) {
                if (ch == '\n') return bytes.toString("US-ASCII").replace("\r", "");
                bytes.write(ch);
                if (bytes.size() > 8192) throw new IOException("Proxy response line too long");
            }
            throw new EOFException("Proxy closed CONNECT response");
        }
        Object start(List<Object> headers, boolean hasBody, BooleanSupplier canceled) throws Exception {
            // Serialize admission, not body I/O. Settings and completed streams
            // belong to the actual framing implementation. A bounded wait also
            // observes settings-only updates, which this pinned provider does
            // not notify on its connection monitor.
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
            for (;;) {
                if (canceled.getAsBoolean()) throw new IOException("Stream closed");
                synchronized (starts) {
                    boolean available;
                    synchronized (connection) {
                        if (!healthy()) throw new IOException("HTTP/2 session shut down");
                        int open = (Integer) Http2Transport.call(connection.getClass(), connection,
                                "openStreamCount", new Class<?>[0]);
                        available = open < streamLimit(connection);
                    }
                    if (available) return Http2Transport.call(connection.getClass(), connection, "newStream",
                            new Class<?>[]{List.class, boolean.class, boolean.class}, headers, hasBody, true);
                }
                synchronized (connection) {
                    if (canceled.getAsBoolean()) throw new IOException("Stream closed");
                    long left = deadline - System.nanoTime();
                    if (left <= 0) throw new SocketTimeoutException("HTTP/2 stream admission timed out");
                    TimeUnit.NANOSECONDS.timedWait(connection,
                            Math.min(left, TimeUnit.MILLISECONDS.toNanos(100)));
                }
            }
        }
        void cancelConnectingOrWake() throws IOException {
            if (!ready) { close(); return; }
            synchronized (connection) { connection.notifyAll(); }
        }
        @Override public void close() throws IOException {
            Socket raw;
            Object framed;
            synchronized (this) {
                if (closed) return;
                closed = true;
                raw = socket;
                framed = connection;
            }
            try { if (raw != null) raw.close(); }
            finally { if (framed != null) ((Closeable) framed).close(); }
        }
    }

    private final List<Session> sessions = new ArrayList<>();
    private final int maxIdle;
    private final long idleNanos;
    private boolean closed;
    private ScheduledFuture<?> expiry;
    Http2SessionPool() { this(5, IDLE_NANOS); }
    Http2SessionPool(int maxIdle, long idleNanos) {
        if (maxIdle < 0 || idleNanos <= 0) throw new IllegalArgumentException();
        this.maxIdle = maxIdle;
        this.idleNanos = idleNanos;
    }
    synchronized Session acquire(Route route) throws Exception {
        if (closed) throw new IOException("HTTP/2 engine closed");
        for (Session session : sessions) {
            if (session.route.equals(route) && session.reusable()
                    && (session.leases != 0 || System.nanoTime() - session.idleSince < idleNanos)) {
                session.leases++;
                scheduleExpiry();
                return session;
            }
        }
        Session session = new Session(route);
        sessions.add(session);
        return session;
    }
    void release(Session session) {
        List<Session> retire = new ArrayList<>();
        synchronized (this) {
            if (--session.leases < 0) throw new IllegalStateException("HTTP/2 lease released twice");
            if (session.leases == 0) {
                session.idleSince = System.nanoTime();
                if (closed || !session.healthy()) {
                    sessions.remove(session);
                    retire.add(session);
                }
            }
            int idle = 0;
            for (Session candidate : sessions) if (candidate.leases == 0) idle++;
            while (idle > maxIdle) {
                Session oldest = null;
                for (Session candidate : sessions) if (candidate.leases == 0
                        && (oldest == null || candidate.idleSince < oldest.idleSince)) oldest = candidate;
                sessions.remove(oldest);
                retire.add(oldest);
                idle--;
            }
            scheduleExpiry();
        }
        for (Session old : retire) closeQuietly(old);
    }
    private void scheduleExpiry() {
        if (expiry != null) { expiry.cancel(false); expiry = null; }
        if (closed) return;
        long oldest = Long.MAX_VALUE;
        for (Session session : sessions) if (session.leases == 0) oldest = Math.min(oldest, session.idleSince);
        if (oldest != Long.MAX_VALUE) expiry = EXPIRY.schedule(this::expire,
                Math.max(0, idleNanos - (System.nanoTime() - oldest)), TimeUnit.NANOSECONDS);
    }
    private void expire() {
        List<Session> retire = new ArrayList<>();
        synchronized (this) {
            long now = System.nanoTime();
            for (Iterator<Session> iterator = sessions.iterator(); iterator.hasNext();) {
                Session session = iterator.next();
                if (session.leases == 0 && (now - session.idleSince >= idleNanos || !session.healthy())) {
                    iterator.remove();
                    retire.add(session);
                }
            }
            scheduleExpiry();
        }
        for (Session session : retire) closeQuietly(session);
    }
    private static void closeQuietly(Session session) {
        try { session.close(); } catch (IOException ignored) { }
    }
    @Override public void close() {
        List<Session> retire;
        synchronized (this) {
            if (closed) return;
            closed = true;
            if (expiry != null) { expiry.cancel(false); expiry = null; }
            retire = new ArrayList<>(sessions);
            sessions.clear();
        }
        for (Session session : retire) closeQuietly(session);
    }
}
