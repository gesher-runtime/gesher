/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

/** Runs the same production/lifecycle/wire corpus on host Java and on Gesher's ART. */
public final class ProxySettingsRuntimeTest {
    private static int checks;
    private static final String[] PROPERTIES = {
        "http.proxyHost", "http.proxyPort", "http.nonProxyHosts",
        "https.proxyHost", "https.proxyPort", "https.nonProxyHosts",
        "socksProxyHost", "socksProxyPort", "socksProxyVersion", "socksNonProxyHosts"
    };

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static void equal(Object actual, Object expected, String message) {
        check(Objects.equals(actual, expected), message + ": expected " + expected + ", got " + actual);
    }

    private static WindowsProxy.Config config(String proxy, String bypass) {
        WindowsProxy.Config value = new WindowsProxy.Config();
        value.proxy = proxy;
        value.bypass = bypass;
        return value;
    }

    private static void apply(WindowsProxy.Config config) throws Exception {
        // The services test DEX and the production app classes can have different
        // loaders; use the declared seam without depending on runtime package identity.
        Method method = WindowsProxy.class.getDeclaredMethod("apply", WindowsProxy.Config.class);
        method.setAccessible(true);
        try { method.invoke(null, config); }
        catch (InvocationTargetException error) { throw new AssertionError("production apply failed", error.getCause()); }
    }

    private static Proxy selected(String uri) {
        List<Proxy> result = ProxySelector.getDefault().select(URI.create(uri));
        check(result.size() == 1, "one proxy selection for " + uri);
        return result.get(0);
    }

    private static void noSocks(String message) {
        for (String property : new String[] {
                "socksProxyHost", "socksProxyPort", "socksProxyVersion", "socksNonProxyHosts"}) {
            equal(System.getProperty(property), null, message + " " + property);
        }
    }

    private static void lifecycle() throws Exception {
        apply(config("socks=proxy.example:1080", "*.internal.example;fileserver"));
        for (int bind = 0; bind < 2; bind++) {
            android.net.Proxy.setHttpProxyConfiguration(null);
            equal(System.getProperty("http.proxyHost"), null, "SOCKS never advertised as HTTP");
            for (String key : new String[] {"http.nonProxyHosts", "https.nonProxyHosts", "socksNonProxyHosts"}) {
                equal(System.getProperty(key), "*.internal.example|fileserver", "bind retains " + key);
            }
            for (String scheme : new String[] {"http", "https", "socket"}) {
                equal(selected(scheme + "://public.example:80/path").type(), Proxy.Type.SOCKS, "ordinary " + scheme);
                equal(selected(scheme + "://fileserver.internal.example:80/path"), Proxy.NO_PROXY, "bypass " + scheme);
            }
        }
        apply(config("socks=proxy.example:1080;http=http-proxy.example:8080", "*.internal.example"));
        equal(selected("http://public.example/").type(), Proxy.Type.HTTP, "HTTP owns HTTP URLs");
        equal(selected("https://public.example/").type(), Proxy.Type.HTTP, "HTTP owns HTTPS URLs");
        equal(selected("socket://public.example:123").type(), Proxy.Type.SOCKS, "explicit SOCKS selection remains available");
        WindowsProxy.Config pac = config("socks=proxy.example:1080", "*.internal.example");
        pac.pacUrl = "http://pac.example/proxy.pac";
        apply(pac);
        noSocks("PAC supersedes static SOCKS");
        check(ProxySelector.getDefault().getClass().getName().equals("android.net.PacProxySelector"), "PAC selector owns routing");
        apply(config("http-proxy.example:8080", "*.new.example"));
        noSocks("HTTP replaces SOCKS");
        equal(System.getProperty("http.nonProxyHosts"), "*.new.example", "HTTP exclusions restored");
        apply(config("", ""));
        for (String property : PROPERTIES) equal(System.getProperty(property), null, "direct clears " + property);
        System.out.println("PASS proxy runtime lifecycle and HTTP/HTTPS/socket selection");
    }

    private static String headers(InputStream input) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        int tail = 0;
        while (tail != 0x0d0a0d0a) {
            int value = input.read();
            if (value < 0) throw new EOFException("HTTP headers truncated");
            bytes.write(value);
            tail = (tail << 8) | value;
            if (bytes.size() > 16384) throw new IOException("HTTP headers too large");
        }
        return bytes.toString(StandardCharsets.US_ASCII.name());
    }

    private static final class HttpOrigin implements AutoCloseable {
        final ServerSocket listener = new ServerSocket();
        final FutureTask<Void> worker;
        volatile String request;
        HttpOrigin(String body) throws IOException {
            listener.bind(new InetSocketAddress("127.0.0.1", 0));
            listener.setSoTimeout(10000);
            worker = new FutureTask<>(() -> {
                try (Socket client = listener.accept()) {
                    client.setSoTimeout(10000);
                    request = headers(client.getInputStream());
                    byte[] payload = body.getBytes(StandardCharsets.US_ASCII);
                    OutputStream output = client.getOutputStream();
                    output.write(("HTTP/1.1 200 OK\r\nContent-Length: " + payload.length
                            + "\r\nConnection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
                    output.write(payload);
                    output.flush();
                }
                return null;
            });
            Thread thread = new Thread(worker, "proxy-test-http-origin");
            thread.setDaemon(true);
            thread.start();
        }
        int port() { return listener.getLocalPort(); }
        void finish() throws Exception { worker.get(15, TimeUnit.SECONDS); }
        @Override public void close() throws IOException { listener.close(); }
    }

    private static final class SocksRelay implements AutoCloseable {
        final ServerSocket listener = new ServerSocket();
        final FutureTask<Void> worker;
        volatile String destination;
        volatile int destinationPort;
        SocksRelay(int targetPort) throws IOException {
            listener.bind(new InetSocketAddress("127.0.0.1", 0));
            listener.setSoTimeout(10000);
            worker = new FutureTask<>(() -> {
                try (Socket client = listener.accept(); Socket target = new Socket(Proxy.NO_PROXY)) {
                    client.setSoTimeout(10000);
                    DataInputStream input = new DataInputStream(client.getInputStream());
                    OutputStream output = client.getOutputStream();
                    if (input.readUnsignedByte() != 5) throw new IOException("not a SOCKS5 greeting");
                    int count = input.readUnsignedByte();
                    byte[] methods = new byte[count]; input.readFully(methods);
                    boolean noAuth = false;
                    for (byte method : methods) if (method == 0) noAuth = true;
                    if (!noAuth) throw new IOException("client omitted no-auth method");
                    output.write(new byte[] {5, 0}); output.flush();
                    if (input.readUnsignedByte() != 5 || input.readUnsignedByte() != 1
                            || input.readUnsignedByte() != 0) throw new IOException("not SOCKS5 CONNECT");
                    int addressType = input.readUnsignedByte();
                    if (addressType != 3) throw new IOException("unresolvable test host must be sent to SOCKS as a domain");
                    byte[] domain = new byte[input.readUnsignedByte()]; input.readFully(domain);
                    destination = new String(domain, StandardCharsets.US_ASCII);
                    destinationPort = input.readUnsignedShort();
                    if (!destination.equals("proxy-target.invalid") || destinationPort != targetPort) {
                        throw new IOException("unexpected SOCKS destination " + destination + ":" + destinationPort);
                    }
                    // This isolated peer maps the test-only name to its real local origin.
                    target.connect(new InetSocketAddress("127.0.0.1", targetPort), 10000);
                    target.setSoTimeout(10000);
                    output.write(new byte[] {5, 0, 0, 1, 127, 0, 0, 1, 0, 0}); output.flush();
                    String request = headers(input);
                    target.getOutputStream().write(request.getBytes(StandardCharsets.US_ASCII));
                    target.getOutputStream().flush();
                    target.getInputStream().transferTo(output);
                    output.flush();
                }
                return null;
            });
            Thread thread = new Thread(worker, "proxy-test-socks-relay");
            thread.setDaemon(true);
            thread.start();
        }
        int port() { return listener.getLocalPort(); }
        void finish() throws Exception { worker.get(15, TimeUnit.SECONDS); }
        @Override public void close() throws IOException { listener.close(); }
    }

    private static String get(String url) throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setConnectTimeout(10000);
        connection.setReadTimeout(10000);
        connection.setInstanceFollowRedirects(false);
        try {
            equal(connection.getResponseCode(), 200, "wire HTTP status");
            try (InputStream input = connection.getInputStream()) {
                return new String(input.readAllBytes(), StandardCharsets.US_ASCII);
            }
        } finally {
            connection.disconnect();
        }
    }

    private static void wire() throws Exception {
        try (HttpOrigin origin = new HttpOrigin("SOCKS_RELAY_OK"); SocksRelay relay = new SocksRelay(origin.port())) {
            apply(config("socks5://127.0.0.1:" + relay.port(), "localhost;127.0.0.1;[::1]"));
            equal(get("http://proxy-target.invalid:" + origin.port() + "/socks-probe"), "SOCKS_RELAY_OK",
                    "HttpURLConnection completed through SOCKS relay");
            origin.finish(); relay.finish();
            equal(relay.destination, "proxy-target.invalid", "SOCKS proxy receives destination hostname");
            check(origin.request.startsWith("GET /socks-probe HTTP/1.1\r\n"), "origin receives HTTP request");
            check(origin.request.contains("Host: proxy-target.invalid:" + origin.port()), "origin Host header preserved");
        }
        // A reachable direct origin must work while the configured SOCKS endpoint is closed.
        int unavailable;
        try (ServerSocket port = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))) {
            unavailable = port.getLocalPort();
        }
        try (HttpOrigin origin = new HttpOrigin("BYPASS_OK")) {
            apply(config("socks=127.0.0.1:" + unavailable, "localhost;127.0.0.1;[::1]"));
            equal(get("http://127.0.0.1:" + origin.port() + "/direct-probe"), "BYPASS_OK", "loopback bypass works with dead SOCKS");
            origin.finish();
        }
        // The two endpoints are simultaneously configured: HTTP must reach its own peer.
        try (HttpOrigin httpProxy = new HttpOrigin("HTTP_PRIORITY_OK")) {
            apply(config("socks=127.0.0.1:" + unavailable + ";http=127.0.0.1:" + httpProxy.port(), ""));
            equal(get("http://proxy-target.invalid/http-priority"), "HTTP_PRIORITY_OK", "HTTP wins over unavailable SOCKS");
            httpProxy.finish();
            check(httpProxy.request.startsWith("GET http://proxy-target.invalid/http-priority HTTP/1.1\r\n"),
                    "HTTP proxy receives absolute-form request");
        }
        apply(config("", ""));
        System.out.println("PASS wire: SOCKS5 greeting/CONNECT/HTTP relay, remote hostname, loopback bypass and HTTP priority");
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("art")) {
            // The standalone ART process needs the framework JNI and its local PAC
            // service. Direct mode keeps the Windows watcher out of this isolated test.
            check(ProxySetting.DIRECT.equals(ProxySetting.ofProcess()),
                    "ART fixture requires GESHER_PROXY=direct");
            System.loadLibrary("android_runtime");
            WindowsProxy.installInProcess();
        }
        Map<String, String> saved = new HashMap<>();
        for (String property : PROPERTIES) saved.put(property, System.getProperty(property));
        ProxySelector selector = ProxySelector.getDefault();
        try {
            lifecycle(); wire();
            System.out.println("PROXY_RUNTIME_CONTRACTS_OK checks=" + checks);
        } finally {
            ProxySelector.setDefault(selector);
            for (String property : PROPERTIES) {
                String value = saved.get(property);
                if (value == null) System.clearProperty(property); else System.setProperty(property, value);
            }
        }
    }
}
