/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.bridgecookies;

import java.io.BufferedInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

/** Ephemeral loopback-only origin. Counts bodies without logging or retaining their contents. */
final class LoopbackServer implements AutoCloseable {
    private final ServerSocket listener;
    private final String hiddenCookie, visibleCookie;
    private final Set<Socket> clients = ConcurrentHashMap.newKeySet();
    final AtomicInteger posts = new AtomicInteger(), bodyBytes = new AtomicInteger();
    final AtomicInteger reservedLeaks = new AtomicInteger();
    final AtomicInteger privatePayloadLeaks = new AtomicInteger();
    private volatile boolean closed;

    LoopbackServer(String suffix) throws IOException {
        hiddenCookie = "bridge_http_" + suffix;
        visibleCookie = "bridge_visible_" + suffix;
        listener = new ServerSocket(0, 16, InetAddress.getByName("127.0.0.1"));
        Thread accept = new Thread(() -> {
            while (!closed) {
                try {
                    Socket client = listener.accept();
                    clients.add(client);
                    Thread worker = new Thread(() -> serve(client), "bridgecookies-local-http");
                    worker.setDaemon(true); worker.start();
                } catch (IOException ended) { if (!closed) BridgeCookiesActivity.fail("loopback-accept"); return; }
            }
        }, "bridgecookies-local-accept");
        accept.setDaemon(true); accept.start();
    }
    String origin() { return "http://127.0.0.1:" + listener.getLocalPort(); }
    String hiddenCookie() { return hiddenCookie; }
    String visibleCookie() { return visibleCookie; }

    private static String line(BufferedInputStream input) throws IOException {
        StringBuilder result = new StringBuilder();
        for (int value; (value = input.read()) >= 0;) {
            if (value == '\n') return result.toString();
            if (value != '\r') result.append((char) value);
            if (result.length() > 16384) throw new IOException("Header too large");
        }
        return result.length() == 0 ? null : result.toString();
    }
    private void serve(Socket socket) {
        try (Socket client = socket) {
            client.setSoTimeout(10000);
            BufferedInputStream input = new BufferedInputStream(client.getInputStream());
            String request = line(input);
            if (request == null) return;
            String[] pieces = request.split(" ", 3);
            if (pieces.length != 3) return;
            String method = pieces[0], path = pieces[1];
            int length = 0;
            for (String header; (header = line(input)) != null && !header.isEmpty();) {
                int colon = header.indexOf(':');
                if (colon > 0 && header.substring(0, colon).equalsIgnoreCase("Content-Length")) {
                    length = Integer.parseInt(header.substring(colon + 1).trim());
                }
            }
            if ("POST".equals(method)) posts.incrementAndGet();
            if (path.startsWith("/.gesher-js-bridge/")) reservedLeaks.incrementAndGet();
            if (length < 0 || length > 4 * 1024 * 1024) throw new IOException("Body length out of bounds");
            byte[] discard = new byte[4096];
            byte[] marker = "fixture-".getBytes(StandardCharsets.US_ASCII);
            int matched = 0;
            while (length > 0) {
                int count = input.read(discard, 0, Math.min(length, discard.length));
                if (count <= 0) throw new IOException("Truncated request");
                bodyBytes.addAndGet(count); length -= count;
                for (int i = 0; i < count; i++) {
                    if (discard[i] == marker[matched]) {
                        if (++matched == marker.length) { privatePayloadLeaks.incrementAndGet(); matched = 0; }
                    } else { matched = discard[i] == marker[0] ? 1 : 0; }
                }
            }
            boolean start = "GET".equals(method) && "/start".equals(path);
            boolean after = "GET".equals(method) && "/after".equals(path);
            String body = start ? page() : after ? "<!doctype html><title>destroyed bridge probe</title>" : "denied";
            String cookies = start ? "Set-Cookie: " + hiddenCookie + "=fixture-hidden; HttpOnly; SameSite=Lax; Path=/\r\n"
                    + "Set-Cookie: " + visibleCookie + "=fixture-visible; SameSite=Lax; Path=/\r\n" : "";
            byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
            String headers = "HTTP/1.1 " + (start || after ? "200 OK" : "403 Forbidden")
                    + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + bytes.length
                    + "\r\nConnection: close\r\nCache-Control: no-store\r\n" + cookies + "\r\n";
            OutputStream output = client.getOutputStream();
            output.write(headers.getBytes(StandardCharsets.US_ASCII)); output.write(bytes); output.flush();
        } catch (Exception error) {
            if (!closed) BridgeCookiesActivity.fail("loopback-request");
        } finally { clients.remove(socket); }
    }

    private String page() {
        return "<!doctype html><meta charset=utf-8><title>local bridge cookie regression</title><script>\n"
                + "var phase='init', endpoint=null, firstBody=null;\n"
                + "var originalOpen=XMLHttpRequest.prototype.open, originalSend=XMLHttpRequest.prototype.send;\n"
                + "XMLHttpRequest.prototype.open=function(m,u,a){this.bridgeFixtureURL=new URL(u,location.href);"
                + "if(this.bridgeFixtureURL.pathname.indexOf('/.gesher-js-bridge/')===0 && !endpoint) endpoint=this.bridgeFixtureURL.pathname;"
                + "return originalOpen.apply(this,arguments);};\n"
                + "XMLHttpRequest.prototype.send=function(body){if(this.bridgeFixtureURL && this.bridgeFixtureURL.pathname===endpoint"
                + " && firstBody===null && typeof body==='string') firstBody=body;return originalSend.apply(this,arguments);};\n"
                + "function denied(method,path,body){var x=new XMLHttpRequest();x.open(method,path,false);x.send(body);"
                + "if(x.status!==403) throw new Error('denial-status');}\n"
                + "window.addEventListener('load',function(){try{\n"
                + "phase='void';var before=performance.now();var v=Fixture.recordVoid(1,'fixture-bridge-argument-do-not-upload');"
                + "if(v!==undefined || performance.now()-before<70) throw new Error('void-not-synchronous');\n"
                + "phase='return';if(Fixture.add(2,20,22)!==42) throw new Error('return-order');\n"
                + "phase='cookie';if(Fixture.cookies(3,document.cookie)!=='http-only-ok') throw new Error('cookie');\n"
                + "phase='large-input';var input='fixture-bridge-argument-do-not-upload:'+'x'.repeat(262144);if(Fixture.large(4,input)!==input.length) throw new Error('input-length');\n"
                + "phase='large-return';var large=Fixture.largeResult(5);if(typeof large!=='string'||large.length!==262144) throw new Error('output-length');\n"
                + "phase='exception';var caught=false;try{Fixture.expectedFailure(6);}catch(e){caught=true;}if(!caught) throw new Error('missing-exception');\n"
                + "phase='recovery';if(Fixture.add(7,1,2)!==3) throw new Error('error-recovery');\n"
                + "phase='transport';if(!endpoint||!firstBody) throw new Error('endpoint-not-captured');\n"
                + "denied('POST',endpoint,firstBody);\n"
                + "var nativeBridge=chrome.webview.hostObjects.sync.gesherBridge;"
                + "var canceled=String(nativeBridge.prepareCall('Fixture','add',JSON.stringify([8,1,2])));"
                + "nativeBridge.finishCall(canceled);denied('POST',endpoint,canceled);\n"
                + "denied('POST','/.gesher-js-bridge/unregistered-fixture-path','fixture-bridge-argument-do-not-upload');\n"
                + "denied('GET',endpoint,null);denied('POST',endpoint+'?fixture=1','fixture-bridge-argument-do-not-upload');\n"
                + "denied('POST',endpoint,new Uint8Array([192,175]));\n"
                + "denied('POST',endpoint,'fixture-bridge-argument-do-not-upload:'+'x'.repeat(1048640));\n"
                + "phase='done';Fixture.finished(8,endpoint,firstBody);\n"
                + "}catch(e){try{Fixture.failed('javascript-'+phase);}catch(ignored){}console.error('bridgecookies FAIL '+phase);}});\n"
                + "</script><p>Only local fixture values are used.</p>";
    }
    @Override public void close() {
        closed = true;
        try { listener.close(); } catch (IOException ignored) { }
        for (Socket client : clients) try { client.close(); } catch (IOException ignored) { }
    }
}
