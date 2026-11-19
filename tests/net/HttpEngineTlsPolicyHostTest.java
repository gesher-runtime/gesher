// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.io.*;
import java.net.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import javax.net.ssl.*;

/** Real TLS/ALPN and pooled provider requests against an independent wire peer.
 * The isolated test trusts its generated loopback certificate through a private
 * trust store. The peer implements GET responses only; no provider code is used.
 */
public class HttpEngineTlsPolicyHostTest {
    private static int checks;
    private static void check(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }
    private static final class Peer implements AutoCloseable {
        final SSLServerSocket listener;
        final ExecutorService workers = Executors.newCachedThreadPool();
        final Set<Socket> sockets = ConcurrentHashMap.newKeySet();
        final AtomicInteger connections = new AtomicInteger();
        final CompletableFuture<Void> failure = new CompletableFuture<>();
        volatile boolean closed;
        Peer(String store) throws Exception {
            KeyStore keys = KeyStore.getInstance("PKCS12");
            try (InputStream in = new FileInputStream(store)) { keys.load(in, "changeit".toCharArray()); }
            KeyManagerFactory managers = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
            managers.init(keys, "changeit".toCharArray());
            SSLContext context = SSLContext.getInstance("TLS");
            context.init(managers.getKeyManagers(), null, null);
            listener = (SSLServerSocket) context.getServerSocketFactory().createServerSocket(
                    0, 8, InetAddress.getByName("127.0.0.1"));
            SSLParameters parameters = listener.getSSLParameters();
            parameters.setApplicationProtocols(new String[]{"h2", "http/1.1"});
            listener.setSSLParameters(parameters);
            workers.execute(() -> {
                try {
                    while (!closed) {
                        SSLSocket socket = (SSLSocket) listener.accept();
                        sockets.add(socket);
                        int id = connections.incrementAndGet();
                        workers.execute(() -> serve(socket, id));
                    }
                } catch (Exception e) { if (!closed) failure.completeExceptionally(e); }
            });
        }
        URL url() throws Exception { return new URL("https://127.0.0.1:" + listener.getLocalPort() + "/policy"); }
        void serve(SSLSocket socket, int id) {
            try (socket) {
                socket.setSoTimeout(10000); socket.startHandshake();
                String protocol = socket.getApplicationProtocol();
                if (protocol.equals("h2")) http2(socket, id);
                else if (protocol.equals("http/1.1")) http1(socket, id);
                else throw new IOException("Client omitted ALPN");
            } catch (EOFException | SocketException expected) {
                // A completed request may retire a pooled connection.
            } catch (Exception e) { if (!closed) failure.completeExceptionally(e); }
            finally { sockets.remove(socket); }
        }
        void http1(Socket socket, int id) throws IOException {
            BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.US_ASCII));
            for (String request; (request = in.readLine()) != null;) {
                if (!request.startsWith("GET ")) throw new IOException("Expected GET");
                for (String header; (header = in.readLine()) != null && !header.isEmpty();) {}
                socket.getOutputStream().write(("HTTP/1.1 200 OK\r\nX-Peer-Protocol: http/1.1\r\n"
                        + "X-Peer-Connection: " + id + "\r\nContent-Length: 4\r\n\r\nPASS")
                        .getBytes(StandardCharsets.US_ASCII));
                socket.getOutputStream().flush();
            }
        }
        static void frame(OutputStream out, int type, int flags, int stream, byte[] payload) throws IOException {
            int length = payload.length;
            out.write(new byte[]{(byte)(length >> 16), (byte)(length >> 8), (byte)length,
                    (byte)type, (byte)flags, (byte)(stream >> 24), (byte)(stream >> 16),
                    (byte)(stream >> 8), (byte)stream});
            out.write(payload); out.flush();
        }
        static void literal(ByteArrayOutputStream out, String name, String value) throws IOException {
            out.write(0); out.write(name.length()); out.write(name.getBytes(StandardCharsets.US_ASCII));
            out.write(value.length()); out.write(value.getBytes(StandardCharsets.US_ASCII));
        }
        void http2(Socket socket, int id) throws IOException {
            InputStream in = socket.getInputStream(); OutputStream out = socket.getOutputStream();
            if (!new String(in.readNBytes(24), StandardCharsets.US_ASCII).equals("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"))
                throw new IOException("Missing HTTP/2 preface");
            frame(out, 4, 0, 0, new byte[0]);
            for (;;) {
                byte[] header = in.readNBytes(9);
                if (header.length == 0) return;
                if (header.length != 9) throw new EOFException();
                int length = (header[0] & 255) << 16 | (header[1] & 255) << 8 | header[2] & 255;
                int type = header[3] & 255, flags = header[4] & 255;
                int stream = ByteBuffer.wrap(header, 5, 4).getInt() & 0x7fffffff;
                byte[] payload = in.readNBytes(length);
                if (payload.length != length) throw new EOFException();
                if (type == 4 && (flags & 1) == 0) frame(out, 4, 1, 0, new byte[0]);
                else if (type == 6 && (flags & 1) == 0) frame(out, 6, 1, 0, payload);
                else if (type == 1) {
                    if ((flags & 4) == 0) throw new IOException("Unexpected split request headers");
                    ByteArrayOutputStream headers = new ByteArrayOutputStream();
                    headers.write(0x88); // HPACK indexed :status = 200.
                    literal(headers, "x-peer-protocol", "h2");
                    literal(headers, "x-peer-connection", Integer.toString(id));
                    frame(out, 1, 4, stream, headers.toByteArray());
                    frame(out, 0, 1, stream, "PASS".getBytes(StandardCharsets.US_ASCII));
                } else if (type == 7) return;
            }
        }
        public void close() throws Exception {
            closed = true; listener.close();
            for (Socket socket : sockets) socket.close();
            workers.shutdownNow(); workers.awaitTermination(5, TimeUnit.SECONDS);
            if (failure.isCompletedExceptionally()) failure.get();
        }
    }
    private static String direct(HttpEngine engine, URL url, String protocol) throws Exception {
        HttpURLConnection connection = (HttpURLConnection) engine.openConnection(url);
        connection.setRequestProperty("X-Android-Protocols", "h2,http/1.1");
        try (InputStream in = connection.getInputStream()) {
            check(new String(in.readAllBytes(), StandardCharsets.US_ASCII).equals("PASS"), "Direct TLS body");
        }
        check(protocol.equals(connection.getHeaderField("X-Peer-Protocol")), "Direct ALPN escaped engine policy");
        return connection.getHeaderField("X-Peer-Connection");
    }
    private static String async(HttpEngine engine, URL url, Executor callbacks, String protocol) throws Exception {
        CompletableFuture<UrlResponseInfo> done = new CompletableFuture<>();
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        UrlRequest request = engine.newUrlRequestBuilder(url.toString(), callbacks, new UrlRequest.Callback() {
            public void onRedirectReceived(UrlRequest r, UrlResponseInfo i, String next) { r.followRedirect(); }
            public void onResponseStarted(UrlRequest r, UrlResponseInfo i) { r.read(ByteBuffer.allocateDirect(32)); }
            public void onReadCompleted(UrlRequest r, UrlResponseInfo i, ByteBuffer b) {
                b.flip(); while (b.hasRemaining()) body.write(b.get()); b.clear(); r.read(b);
            }
            public void onSucceeded(UrlRequest r, UrlResponseInfo i) { done.complete(i); }
            public void onFailed(UrlRequest r, UrlResponseInfo i, HttpException e) { done.completeExceptionally(e); }
            public void onCanceled(UrlRequest r, UrlResponseInfo i) { done.completeExceptionally(new IOException("Canceled")); }
        }).addHeader("X-Android-Protocols", "h2,http/1.1").build();
        try {
            request.start(); UrlResponseInfo response = done.get(10, TimeUnit.SECONDS);
            check(body.toString("US-ASCII").equals("PASS"), "Async TLS body");
            check(protocol.equals(response.getNegotiatedProtocol()), "Incorrect negotiated protocol");
            check(Collections.singletonList(protocol).equals(response.getHeaders().getAsMap().get("X-Peer-Protocol")),
                    "Async ALPN escaped engine policy");
            return response.getHeaders().getAsMap().get("X-Peer-Connection").get(0);
        } finally { request.cancel(); }
    }
    public static void main(String[] args) throws Exception {
        System.setProperty("java.protocol.handler.pkgs", "gesher.http.policy");
        HttpEngine.Builder builder = new HttpEngine.Builder(new android.content.ContextWrapper(null));
        HttpEngine enabled = builder.setEnableHttp2(true).build(), disabled = builder.setEnableHttp2(false).build();
        HttpEngine installed = args[1].equals("global-enabled") ? enabled : disabled;
        URL.setURLStreamHandlerFactory(installed.createUrlStreamHandlerFactory());
        ExecutorService callbacks = Executors.newSingleThreadExecutor();
        try (Peer peer = new Peer(args[0])) {
            String h2 = direct(enabled, peer.url(), "h2");
            String h1 = direct(disabled, peer.url(), "http/1.1");
            check(!h2.equals(h1), "Different protocol policies shared a TLS connection");
            check(h1.equals(async(disabled, peer.url(), callbacks, "http/1.1")), "HTTP/1.1 connection was not reused");
            check(h2.equals(async(enabled, peer.url(), callbacks, "h2")), "HTTP/2 connection was not reused");
            installed.shutdown();
            HttpEngine survivor = installed == enabled ? disabled : enabled;
            String expected = survivor == enabled ? "h2" : "http/1.1";
            direct(survivor, peer.url(), expected); async(survivor, peer.url(), callbacks, expected);
            HttpEngine later = builder.setEnableHttp2(installed != enabled).build();
            try { direct(later, peer.url(), expected); async(later, peer.url(), callbacks, expected); }
            finally { later.shutdown(); }
        } finally { callbacks.shutdownNow(); enabled.shutdown(); disabled.shutdown(); }
        System.out.println("HttpEngineTlsPolicyHostTest passed: " + checks + " TLS/ALPN and pooled connection checks (" + args[1] + ")");
    }
}
