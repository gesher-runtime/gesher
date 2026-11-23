// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.io.*;
import java.lang.reflect.Field;
import java.net.*;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

/** Public connection ownership over the actual Android HTTP provider and loopback sockets. */
public final class HttpEngineConnectionLifetimeTest {
    private static int checks;
    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }
    private static HttpEngine engine() {
        return new HttpEngine.Builder(new android.content.ContextWrapper(null))
                .setEnableHttpCache(HttpEngine.Builder.HTTP_CACHE_IN_MEMORY, 128 * 1024).build();
    }
    private static void active(HttpEngine engine, String operation) {
        boolean rejected = false;
        try { engine.shutdown(); } catch (IllegalStateException expected) { rejected = true; }
        check(rejected, "shutdown accepted active " + operation);
    }
    private static String body(InputStream in) throws IOException {
        check(in != null, "Missing response stream");
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] bytes = new byte[3];
        for (int count; (count = in.read(bytes)) != -1;) out.write(bytes, 0, count);
        check(in.read() == -1, "Repeated EOF changed");
        return out.toString("US-ASCII");
    }
    private static final class Peer implements AutoCloseable {
        final ServerSocket server = new ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"));
        final BlockingQueue<String> requests = new LinkedBlockingQueue<>();
        final AtomicReference<Throwable> failure = new AtomicReference<>();
        final CountDownLatch blockedHeaders = new CountDownLatch(1);
        final CountDownLatch blockedBody = new CountDownLatch(1);
        final CountDownLatch peerClosed = new CountDownLatch(1);
        final Thread worker;
        volatile boolean closed, cancelRequested;
        Peer() throws IOException {
            worker = new Thread(() -> {
                try {
                    while (!closed) try (Socket socket = server.accept()) {
                        socket.setSoTimeout(5000);
                        BufferedReader reader = new BufferedReader(new InputStreamReader(socket.getInputStream(), "US-ASCII"));
                        String request = reader.readLine();
                        if (request == null) continue;
                        int size = 0;
                        for (String line; (line = reader.readLine()) != null && !line.isEmpty();) {
                            if (line.toLowerCase(Locale.ROOT).startsWith("content-length:"))
                                size = Integer.parseInt(line.substring(line.indexOf(':') + 1).trim());
                        }
                        for (int i = 0; i < size; i++) if (reader.read() == -1) throw new EOFException("Upload ended early");
                        requests.add(request);
                        if (request.contains(" /blocked-headers ")) {
                            blockedHeaders.countDown();
                            checkPeerEof(reader);
                            continue;
                        }
                        if (request.contains(" /malformed ")) {
                            socket.getOutputStream().write("This is not HTTP\r\n\r\n".getBytes("US-ASCII"));
                            continue;
                        }
                        boolean error = request.contains(" /error ");
                        boolean head = request.startsWith("HEAD ");
                        boolean empty = request.contains(" /empty ");
                        boolean noContent = request.contains(" /204 ");
                        boolean notModified = request.contains(" /304 ");
                        boolean redirect = request.contains(" /redirect ");
                        boolean blocked = request.contains(" /blocked-body ");
                        boolean truncated = request.contains(" /truncated ");
                        String status = error ? "404 Not Found" : redirect ? "302 Found" : noContent ? "204 No Content"
                                : notModified ? "304 Not Modified" : "200 OK";
                        int length = empty ? 0 : blocked || truncated ? 10000 : 4;
                        String headers = "HTTP/1.1 " + status + "\r\nConnection: close\r\n"
                                + "Cache-Control: public, max-age=600\r\nContent-Type: application/octet-stream\r\n"
                                + (redirect ? "Location: /after\r\n" : "")
                                + (noContent || notModified ? "" : "Content-Length: " + length + "\r\n")
                                + "\r\n";
                        OutputStream out = socket.getOutputStream();
                        out.write(headers.getBytes("US-ASCII"));
                        if (!head && !empty && !noContent && !notModified) out.write("PASS".getBytes("US-ASCII"));
                        out.flush();
                        if (blocked) { blockedBody.countDown(); checkPeerEof(reader); }
                    }
                } catch (Throwable error) {
                    if (!(closed && error instanceof SocketException)) failure.compareAndSet(null, error);
                }
            }, "http-connection-lifetime-peer");
            worker.setDaemon(true); worker.start();
        }
        private void checkPeerEof(Reader reader) throws IOException {
            int value;
            try { value = reader.read(); }
            catch (SocketException error) { if (!cancelRequested) throw error; value = -1; }
            if (value != -1) throw new IOException("Unexpected client bytes while waiting for cancellation");
            if (!cancelRequested) throw new IOException("Peer closed before cancellation was requested");
            peerClosed.countDown();
        }
        URL url(String path) throws MalformedURLException {
            return new URL("http://127.0.0.1:" + server.getLocalPort() + path);
        }
        HttpURLConnection connection(HttpEngine engine, String path) throws IOException {
            HttpURLConnection result = (HttpURLConnection) engine.openConnection(url(path));
            result.setConnectTimeout(5000); result.setReadTimeout(5000);
            return result;
        }
        void request() throws Exception {
            check(requests.poll(5, TimeUnit.SECONDS) != null, "No real peer request");
            healthy();
        }
        void healthy() { if (failure.get() != null) throw new AssertionError("Peer failed", failure.get()); }
        public void close() throws Exception {
            healthy(); closed = true; server.close(); worker.join(5000);
            check(!worker.isAlive(), "Peer did not retire"); healthy();
        }
    }
    private static void ordinary(Peer peer, String entry) throws Exception {
        HttpEngine engine = engine();
        HttpURLConnection connection = peer.connection(engine, "/" + entry);
        InputStream stream = null;
        try {
            switch (entry) {
                case "connect": connection.connect(); break;
                case "code": check(connection.getResponseCode() == 200, "Response code"); break;
                case "message": check("OK".equals(connection.getResponseMessage()), "Response message"); break;
                case "header": check("4".equals(connection.getHeaderField("Content-Length")), "Named header"); break;
                case "headers": check(!connection.getHeaderFields().isEmpty(), "Header map"); break;
                case "index": check(connection.getHeaderField(0) != null, "Indexed header"); break;
                case "key": check(connection.getHeaderFieldKey(0) != null, "Indexed key"); break;
                case "input": stream = connection.getInputStream(); break;
                case "content": stream = (InputStream) connection.getContent(); break;
                case "typed": stream = (InputStream) connection.getContent(new Class<?>[]{InputStream.class}); break;
                case "upload":
                    connection.setDoOutput(true); connection.setFixedLengthStreamingMode(4);
                    try (OutputStream output = connection.getOutputStream()) { output.write(new byte[]{1, 2, 3, 4}); }
                    break;
                case "get-upload-error":
                    boolean rejected = false;
                    try { connection.getOutputStream(); } catch (ProtocolException expected) { rejected = true; }
                    check(rejected, "GET output must fail locally");
                    break;
                default: throw new AssertionError(entry);
            }
            active(engine, entry);
            if (stream == null) stream = connection.getInputStream();
            check("PASS".equals(body(stream)), "Response changed through " + entry);
            peer.request();
            engine.shutdown();
            check(connection.getResponseCode() == 200 && "4".equals(connection.getHeaderField("Content-Length")),
                    "Completed response metadata depends on a live engine");
            stream.close(); connection.disconnect();
        } finally { connection.disconnect(); engine.shutdown(); }
    }
    private static void errors(Peer peer) throws Exception {
        for (boolean inputFirst : new boolean[]{false, true}) {
            HttpEngine engine = engine();
            HttpURLConnection connection = peer.connection(engine, "/error");
            try {
                if (inputFirst) {
                    boolean notFound = false;
                    try { connection.getInputStream(); } catch (FileNotFoundException expected) { notFound = true; }
                    check(notFound, "HTTP error was not preserved");
                }
                InputStream error = connection.getErrorStream();
                active(engine, "error body");
                check("PASS".equals(body(error)), "Error body changed"); peer.request(); engine.shutdown();
                check(connection.getResponseCode() == 404, "Error headers lost after EOF");
            } finally { connection.disconnect(); engine.shutdown(); }
        }
    }
    private static void empty(Peer peer) throws Exception {
        for (String path : new String[]{"/empty", "/204", "/304", "/head"}) {
            HttpEngine engine = engine();
            HttpURLConnection connection = peer.connection(engine, path);
            try {
                if (path.equals("/head")) connection.setRequestMethod("HEAD");
                int code = connection.getResponseCode();
                check(code == 200 || code == 204 || code == 304, "Empty response code");
                peer.request(); engine.shutdown();
                check(connection.getInputStream().read() == -1, "Empty response not at EOF");
            } finally { connection.disconnect(); engine.shutdown(); }
        }
    }
    private static void failed(Peer peer) throws Exception {
        HttpEngine engine = engine();
        HttpURLConnection malformed = peer.connection(engine, "/malformed");
        try {
            check(malformed.getHeaderFields().isEmpty(), "Malformed response must not produce headers");
            peer.request(); engine.shutdown();
        } finally { malformed.disconnect(); engine.shutdown(); }
        engine = engine();
        HttpURLConnection truncated = peer.connection(engine, "/truncated");
        try {
            InputStream stream = truncated.getInputStream(); active(engine, "truncated body before read");
            boolean failed = false;
            try { body(stream); } catch (IOException expected) { failed = true; }
            check(failed, "Truncated response silently succeeded"); peer.request(); engine.shutdown();
        } finally { truncated.disconnect(); engine.shutdown(); }
        engine = engine();
        int port;
        try (ServerSocket socket = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))) { port = socket.getLocalPort(); }
        HttpURLConnection refused = (HttpURLConnection) engine.openConnection(new URL("http://127.0.0.1:" + port + "/"));
        refused.setConnectTimeout(1000);
        try {
            boolean failed = false;
            try { refused.connect(); } catch (IOException expected) { failed = true; }
            check(failed, "Connection to closed peer unexpectedly succeeded"); engine.shutdown();
        } finally { refused.disconnect(); engine.shutdown(); }
    }
    private static void cancellation(String path) throws Exception {
        try (Peer peer = new Peer()) {
            HttpEngine engine = engine();
            HttpURLConnection connection = peer.connection(engine, path);
            AtomicReference<Throwable> unexpected = new AtomicReference<>();
            CountDownLatch reading = new CountDownLatch(1);
            Thread worker = new Thread(() -> {
                try {
                    if (path.equals("/blocked-headers")) { reading.countDown(); connection.getResponseCode(); }
                    else {
                        InputStream input = connection.getInputStream();
                        byte[] prefix = new byte[4];
                        for (int i = 0; i < prefix.length;) {
                            int count = input.read(prefix, i, prefix.length - i);
                            if (count < 0) throw new AssertionError("Missing body prefix");
                            i += count;
                        }
                        reading.countDown(); input.read();
                    }
                    unexpected.set(new AssertionError("Blocked operation succeeded after disconnect"));
                } catch (IOException expected) {
                    if (!peer.cancelRequested) unexpected.set(expected);
                }
                catch (Throwable error) { unexpected.set(error); }
            }, "http-connection-lifetime-reader");
            try {
                worker.start();
                check(reading.await(5, TimeUnit.SECONDS), "Reader did not start");
                check((path.equals("/blocked-headers") ? peer.blockedHeaders : peer.blockedBody)
                        .await(5, TimeUnit.SECONDS), "Peer did not reach blocked response");
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                boolean observed = false;
                String expectedClass = "com.android.okhttp.internal.http.Http1xStream"
                        + (path.equals("/blocked-headers") ? "" : "$FixedLengthSource");
                String expectedMethod = path.equals("/blocked-headers") ? "readResponse" : "read";
                while (worker.isAlive() && System.nanoTime() < deadline) {
                    boolean provider = false, socket = false;
                    for (StackTraceElement frame : worker.getStackTrace()) {
                        if (frame.getClassName().equals(expectedClass) && frame.getMethodName().equals(expectedMethod)) provider = true;
                        if ((frame.getClassName().startsWith("java.net.") || frame.getClassName().startsWith("sun.nio.ch.")
                                || frame.getClassName().startsWith("libcore.io."))
                                && frame.getMethodName().toLowerCase(Locale.ROOT).contains("read")) socket = true;
                    }
                    if (provider && socket) { observed = true; break; }
                    Thread.sleep(5);
                }
                check(observed, "Did not observe a production socket read blocked in " + expectedClass);
                active(engine, path);
                peer.cancelRequested = true;
                connection.disconnect(); worker.join(5000);
                check(!worker.isAlive(), "Canceled production call did not retire");
                check(unexpected.get() == null, "Unexpected canceled result: " + unexpected.get());
                check(peer.peerClosed.await(5, TimeUnit.SECONDS), "Canceled transport did not close");
                peer.request(); engine.shutdown();
            } finally { connection.disconnect(); worker.join(5000); engine.shutdown(); }
        }
    }
    private static void earlyClose() throws Exception {
        try (Peer peer = new Peer()) {
            HttpEngine engine = engine();
            HttpURLConnection connection = peer.connection(engine, "/blocked-body");
            try {
                InputStream stream = connection.getInputStream();
                check(stream.read() == 'P', "Missing early-close response prefix");
                active(engine, "partially consumed response");
                peer.cancelRequested = true;
                stream.close();
                engine.shutdown();
                check(peer.peerClosed.await(5, TimeUnit.SECONDS), "Early-close transport did not retire");
                peer.request();
            } finally { connection.disconnect(); engine.shutdown(); }
        }
    }
    /** A scheduling gate around the real response stream, never a substitute response. */
    private static final class GatedInput extends FilterInputStream {
        final CountDownLatch entered=new CountDownLatch(1), release=new CountDownLatch(1);
        final boolean blockRead;
        GatedInput(InputStream source, boolean blockRead) { super(source); this.blockRead=blockRead; }
        private void gate() throws IOException {
            entered.countDown();
            try { if (!release.await(10,TimeUnit.SECONDS)) throw new IOException("Fixture gate timed out"); }
            catch (InterruptedException error) { Thread.currentThread().interrupt(); throw new IOException(error); }
        }
        @Override public int read(byte[] bytes,int offset,int length) throws IOException {
            if (blockRead) gate();
            return in.read(bytes,offset,length);
        }
        @Override public void close() throws IOException {
            if (!blockRead) gate();
            in.close();
        }
    }
    private static GatedInput gateBody(UrlRequest request, boolean blockRead, boolean redirect) throws Exception {
        synchronized (request) {
            Field input=request.getClass().getDeclaredField("input"); input.setAccessible(true);
            Field transports=request.getClass().getDeclaredField("transports"); transports.setAccessible(true);
            @SuppressWarnings("unchecked") Map<HttpURLConnection,InputStream> owned=
                    (Map<HttpURLConnection,InputStream>)transports.get(request);
            InputStream body=redirect ? owned.values().iterator().next() : (InputStream)input.get(request);
            if (body==null) throw new AssertionError("No actual provider response to gate");
            GatedInput gate=new GatedInput(body,blockRead);
            for (Map.Entry<HttpURLConnection,InputStream> entry:owned.entrySet()) {
                if (entry.getValue()==body) entry.setValue(gate);
            }
            if (!redirect) input.set(request,gate);
            return gate;
        }
    }
    private static void asynchronousOwnership(String mode) throws Exception {
        try (Peer peer=new Peer()) {
            HttpEngine engine=engine();
            ExecutorService executor=Executors.newSingleThreadExecutor();
            CompletableFuture<GatedInput> ready=new CompletableFuture<>();
            CompletableFuture<Void> terminal=new CompletableFuture<>();
            AtomicReference<Throwable> unexpected=new AtomicReference<>();
            boolean cancel=mode.equals("read-cancel") || mode.equals("redirect-cancel");
            UrlRequest request=engine.newUrlRequestBuilder(peer.url(mode.equals("redirect-cancel")
                    ? "/redirect" : "/async").toString(),executor,new UrlRequest.Callback() {
                private void terminal(String outcome) {
                    try {
                        if (!(cancel ? outcome.equals("canceled") : outcome.equals("success")))
                            throw new AssertionError("Unexpected asynchronous outcome: "+outcome);
                        // The terminal callback is the first point promising that
                        // all production I/O and cache stream cleanup has retired.
                        engine.shutdown();
                        terminal.complete(null);
                    } catch(Throwable error) { unexpected.compareAndSet(null,error); terminal.completeExceptionally(error); }
                }
                public void onRedirectReceived(UrlRequest r,UrlResponseInfo i,String url) {
                    try {
                        GatedInput gate=gateBody(r,false,true); ready.complete(gate);
                        r.followRedirect(); r.cancel();
                    } catch(Exception error) { unexpected.compareAndSet(null,error); ready.completeExceptionally(error); r.cancel(); }
                }
                public void onResponseStarted(UrlRequest r,UrlResponseInfo i) {
                    try { ready.complete(gateBody(r,mode.equals("read-cancel"),false)); r.read(ByteBuffer.allocateDirect(8)); }
                    catch(Exception error) { unexpected.compareAndSet(null,error); ready.completeExceptionally(error); r.cancel(); }
                }
                public void onReadCompleted(UrlRequest r,UrlResponseInfo i,ByteBuffer buffer) { buffer.clear(); r.read(buffer); }
                public void onSucceeded(UrlRequest r,UrlResponseInfo i) { terminal("success"); }
                public void onFailed(UrlRequest r,UrlResponseInfo i,HttpException error) { terminal("failed: "+error); }
                public void onCanceled(UrlRequest r,UrlResponseInfo i) { terminal("canceled"); }
            }).build();
            GatedInput gate=null;
            try {
                request.start(); gate=ready.get(5,TimeUnit.SECONDS);
                check(gate.entered.await(5,TimeUnit.SECONDS),"No production "+mode+" owner reached its gate");
                if (mode.equals("read-cancel")) request.cancel();
                active(engine,"asynchronous "+mode+" resource retirement");
                check(!terminal.isDone(),"Terminal callback escaped before "+mode+" cleanup");
                gate.release.countDown(); terminal.get(5,TimeUnit.SECONDS);
                check(request.isDone(),"Request did not remain terminal");
                CompletableFuture<Void> barrier=new CompletableFuture<>(); executor.execute(()->barrier.complete(null));
                barrier.get(5,TimeUnit.SECONDS);
                check(unexpected.get()==null,"Late asynchronous failure: "+unexpected.get());
                peer.request();
                check(peer.requests.isEmpty(),"Canceled redirect started another network hop");
            } finally {
                if (gate!=null) gate.release.countDown();
                request.cancel(); executor.shutdown();
                check(executor.awaitTermination(5,TimeUnit.SECONDS),"Callback executor did not retire");
                engine.shutdown();
            }
        }
    }
    public static void main(String[] args) throws Exception {
        if (Arrays.asList(args).contains("--host")) System.setProperty("java.protocol.handler.pkgs", "gesher.http.policy");
        try (Peer peer = new Peer()) {
            HttpEngine unused = engine();
            HttpURLConnection late = peer.connection(unused, "/late");
            unused.shutdown();
            boolean rejected = false;
            try { late.getResponseCode(); } catch (IllegalStateException expected) { rejected = true; }
            check(rejected && peer.requests.isEmpty(), "Unused connection started after shutdown");
            HttpEngine fresh = engine();
            HttpURLConnection before = peer.connection(fresh, "/before");
            before.disconnect();
            check("PASS".equals(body(before.getInputStream())), "Disconnect before start changed connection");
            peer.request(); fresh.shutdown();
            for (String entry : new String[]{"connect", "code", "message", "header", "headers", "index", "key",
                    "input", "content", "typed", "upload", "get-upload-error"}) ordinary(peer, entry);
            errors(peer); empty(peer); failed(peer);
        }
        cancellation("/blocked-headers"); cancellation("/blocked-body");
        earlyClose();
        asynchronousOwnership("read-cancel");
        asynchronousOwnership("success-close");
        asynchronousOwnership("redirect-cancel");
        System.out.println("HttpEngineConnectionLifetimeTest passed: " + checks + " actual-provider ownership checks");
    }
}
