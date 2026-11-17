// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.io.*;
import java.lang.reflect.Field;
import java.net.*;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;

/** Actual Android OkHttp connection policy plus ordinary loopback HTTP metadata. */
public class HttpEngineProtocolPolicyTest {
    private static int checks;
    private static void check(boolean ok, String message) {
        checks++;
        if (!ok) throw new AssertionError(message);
    }
    private static void policy(URLConnection connection, boolean enabled) throws Exception {
        Object nativeConnection = connection;
        // The public wire assertions below are complemented by checking the actual
        // provider's ALPN list, without substituting an OkHttp connection fixture.
        if (nativeConnection.getClass().getName().equals("android.net.http.JavaEngineConnection")) {
            Field field = nativeConnection.getClass().getDeclaredField("delegate");
            field.setAccessible(true); nativeConnection = field.get(nativeConnection);
        }
        if (nativeConnection.getClass().getName().equals("com.android.okhttp.internal.huc.HttpsURLConnectionImpl")) {
            Field field = nativeConnection.getClass().getDeclaredField("delegate");
            field.setAccessible(true); nativeConnection = field.get(nativeConnection);
        }
        Field field = nativeConnection.getClass().getDeclaredField("client");
        field.setAccessible(true);
        Object client = field.get(nativeConnection);
        String actual = client.getClass().getMethod("getProtocols").invoke(client).toString();
        check(actual.equals(enabled ? "[h2, http/1.1]" : "[http/1.1]"),
                "Provider protocol list escaped engine policy: " + actual);
    }
    private static String body(InputStream stream) throws IOException {
        try (InputStream in = stream; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] bytes = new byte[128];
            for (int read; (read = in.read(bytes)) != -1;) out.write(bytes, 0, read);
            return out.toString("US-ASCII");
        }
    }
    private static final class Peer implements AutoCloseable {
        final ServerSocket listener = new ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"));
        final BlockingQueue<Map<String, String>> requests = new LinkedBlockingQueue<>();
        final CompletableFuture<Void> failure = new CompletableFuture<>();
        final Thread thread;
        volatile boolean closed;
        Peer() throws IOException {
            thread = new Thread(() -> {
                try {
                    while (!closed) try (Socket socket = listener.accept()) {
                        socket.setSoTimeout(5000);
                        BufferedReader input = new BufferedReader(new InputStreamReader(socket.getInputStream(), "US-ASCII"));
                        String line = input.readLine();
                        Map<String, String> headers = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
                        headers.put(":request", line);
                        for (String h; (h = input.readLine()) != null && !h.isEmpty();) {
                            int colon = h.indexOf(':');
                            headers.put(h.substring(0, colon), h.substring(colon + 1).trim());
                        }
                        int length = Integer.parseInt(headers.getOrDefault("Content-Length", "0"));
                        char[] upload = new char[length];
                        for (int offset = 0; offset < length;) {
                            int read = input.read(upload, offset, length - offset);
                            if (read < 0) throw new EOFException();
                            offset += read;
                        }
                        headers.put(":body", new String(upload)); requests.add(headers);
                        String response = line.contains(" /redirect ")
                                ? "HTTP/1.1 307 Temporary Redirect\r\nLocation: /final\r\nContent-Length: 0\r\n"
                                : line.contains(" /error ") ? "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n"
                                : "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n";
                        socket.getOutputStream().write((response + "Connection: close\r\n\r\n" +
                                (line.contains(" /redirect ") ? "" : "PASS")).getBytes("US-ASCII"));
                    }
                } catch (Throwable e) { if (!closed) failure.completeExceptionally(e); }
            }, "http-policy-peer");
            thread.setDaemon(true); thread.start();
        }
        URL url(String path) throws Exception { return new URL("http://127.0.0.1:" + listener.getLocalPort() + path); }
        Map<String, String> request() throws Exception {
            Map<String, String> value = requests.poll(5, TimeUnit.SECONDS);
            if (failure.isCompletedExceptionally()) failure.get();
            check(value != null, "No request reached the actual peer"); return value;
        }
        public void close() throws Exception { closed = true; listener.close(); thread.join(5000); }
    }
    public static void main(String[] args) throws Exception {
        List<String> modes = Arrays.asList(args);
        if (modes.contains("--host")) System.setProperty("java.protocol.handler.pkgs", "gesher.http.policy");
        HttpEngine.Builder builder = new HttpEngine.Builder(new android.content.ContextWrapper(null));
        HttpEngine disabled = builder.setEnableHttp2(false).build();
        HttpEngine enabled = builder.setEnableHttp2(true).build();
        HttpEngine installed = modes.contains("global-enabled") ? enabled
                : modes.contains("global-disabled") ? disabled : null;
        if (installed != null) URL.setURLStreamHandlerFactory(installed.createUrlStreamHandlerFactory());
        ExecutorService callbacks = Executors.newSingleThreadExecutor();
        try (Peer peer = new Peer()) {
            for (boolean enable : new boolean[]{false, true}) {
                HttpURLConnection connection = (HttpURLConnection) (enable ? enabled : disabled).openConnection(peer.url("/direct"));
                connection.setRequestProperty("X-Android-Protocols", "h2,http/1.1");
                connection.addRequestProperty("X-Android-Transports", "h2");
                check("h2,http/1.1".equals(connection.getRequestProperty("x-android-protocols")), "Application header disappeared");
                policy(connection, enable);
                connection.setConnectTimeout(5000); connection.setReadTimeout(5000);
                check(connection.getConnectTimeout() == 5000 && connection.getReadTimeout() == 5000, "Timeout delegation");
                connection.setInstanceFollowRedirects(false);
                check(!connection.getInstanceFollowRedirects(), "Redirect delegation");
                connection.setDoOutput(true); connection.setRequestMethod("POST");
                connection.setFixedLengthStreamingMode(4L);
                try (OutputStream out = connection.getOutputStream()) { out.write("BODY".getBytes("US-ASCII")); }
                check(connection.getResponseCode() == 200 && body(connection.getInputStream()).equals("PASS"), "Direct response");
                Map<String, String> actual = peer.request();
                check("h2,http/1.1".equals(actual.get("X-Android-Protocols")) && "h2".equals(actual.get("X-Android-Transports")), "Wire headers were changed or dropped");
                check(actual.get(":request").startsWith("POST ") && actual.get(":body").equals("BODY"), "Streaming delegation");
                boolean rejected = false;
                try { connection.setRequestProperty("X-Android-Protocols", "later"); }
                catch (IllegalStateException expected) { rejected = true; }
                check(rejected, "Header mutation after connect must fail"); connection.disconnect();
            }
            URL factoryUrl = new URL(null, peer.url("/factory").toString(), disabled.createUrlStreamHandlerFactory().createURLStreamHandler("http"));
            HttpURLConnection viaFactory = (HttpURLConnection) factoryUrl.openConnection();
            viaFactory.setRequestProperty("X-Android-Protocols", "ordinary metadata"); policy(viaFactory, false);
            check(body(viaFactory.getInputStream()).equals("PASS"), "Factory response");
            check("ordinary metadata".equals(peer.request().get("X-Android-Protocols")), "Factory wire header"); viaFactory.disconnect();

            CompletableFuture<Void> complete = new CompletableFuture<>();
            UrlRequest request = disabled.newUrlRequestBuilder(peer.url("/redirect").toString(), callbacks, new UrlRequest.Callback() {
                public void onRedirectReceived(UrlRequest r, UrlResponseInfo i, String url) {
                    if (!Collections.singletonList("/final").equals(i.getHeaders().getAsMap().get("Location")))
                        throw new IllegalStateException("First response header disappeared");
                    r.followRedirect();
                }
                public void onResponseStarted(UrlRequest r, UrlResponseInfo i) {
                    if (!i.getNegotiatedProtocol().equals("http/1.1")) throw new IllegalStateException("Fabricated protocol");
                    r.read(ByteBuffer.allocateDirect(32));
                }
                public void onReadCompleted(UrlRequest r, UrlResponseInfo i, ByteBuffer b) { b.clear(); r.read(b); }
                public void onSucceeded(UrlRequest r, UrlResponseInfo i) { complete.complete(null); }
                public void onFailed(UrlRequest r, UrlResponseInfo i, HttpException error) { complete.completeExceptionally(error); }
                public void onCanceled(UrlRequest r, UrlResponseInfo i) { complete.completeExceptionally(new AssertionError("Canceled")); }
            }).addHeader("X-Android-Protocols", "ordinary metadata")
                    .addHeader("X-Android-Transports", "application value").build();
            request.start(); complete.get(10, TimeUnit.SECONDS);
            for (int hop = 0; hop < 2; ++hop) {
                Map<String, String> actual = peer.request();
                check("ordinary metadata".equals(actual.get("X-Android-Protocols")) &&
                        "application value".equals(actual.get("X-Android-Transports")), "Async redirect lost application headers");
            }
            HttpURLConnection error = (HttpURLConnection) disabled.openConnection(peer.url("/error"));
            check(error.getResponseCode() == 404 && body(error.getErrorStream()).equals("PASS"), "Error stream delegation");
            peer.request(); error.disconnect();

            ProxySelector previous = ProxySelector.getDefault();
            ProxySelector.setDefault(new ProxySelector() {
                public List<Proxy> select(URI uri) {
                    return Collections.singletonList(new Proxy(Proxy.Type.HTTP, peer.listener.getLocalSocketAddress()));
                }
                public void connectFailed(URI uri, SocketAddress address, IOException error) {}
            });
            try {
                HttpURLConnection proxied = (HttpURLConnection) disabled.openConnection(new URL("http://policy.invalid/proxy"));
                check(body(proxied.getInputStream()).equals("PASS") && proxied.usingProxy(), "Platform proxy selection was bypassed");
                check(peer.request().get(":request").startsWith("GET http://policy.invalid/proxy "), "HTTP proxy received the wrong target");
                proxied.disconnect();
            } finally { ProxySelector.setDefault(previous); }

            for (String scheme : new String[]{"http", "https"}) {
                URLStreamHandler handler = disabled.createUrlStreamHandlerFactory().createURLStreamHandler(scheme);
                URL factoryDefault = new URL(null, scheme + "://localhost/default", handler);
                check(factoryDefault.getDefaultPort() == (scheme.equals("http") ? 80 : 443), "Factory default port");
                if (installed != null) check(new URL(scheme + "://localhost/default").getDefaultPort()
                        == (scheme.equals("http") ? 80 : 443), "Global factory default port");
                policy(disabled.openConnection(new URL(scheme + "://localhost/default")), false);
                policy(enabled.openConnection(new URL(scheme + "://localhost/default")), true);
            }
            if (installed != null) {
                installed.shutdown();
                HttpEngine independent = installed == enabled ? disabled : enabled;
                HttpURLConnection afterShutdown = (HttpURLConnection) independent.openConnection(peer.url("/independent"));
                policy(afterShutdown, independent == enabled);
                check(body(afterShutdown.getInputStream()).equals("PASS"), "Another engine's shutdown broke direct requests");
                peer.request(); afterShutdown.disconnect();
            }
        } finally { callbacks.shutdownNow(); disabled.shutdown(); enabled.shutdown(); }
        System.out.println("HttpEngineProtocolPolicyTest passed: " + checks + " actual provider and wire checks");
    }
}
