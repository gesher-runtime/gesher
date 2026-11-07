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

/** Isolated worker scripts; origin requests are counted without retaining their bodies. */
final class WorkerServer implements AutoCloseable {
    private final ServerSocket listener;
    private final Set<Socket> clients = ConcurrentHashMap.newKeySet();
    final AtomicInteger posts = new AtomicInteger(), bodyBytes = new AtomicInteger();
    final AtomicInteger reserved = new AtomicInteger();
    final AtomicInteger privatePayloadLeaks = new AtomicInteger();
    private volatile boolean closed;

    WorkerServer() throws IOException {
        listener = new ServerSocket(0, 16, InetAddress.getByName("127.0.0.1"));
        Thread accept = new Thread(() -> {
            while (!closed) {
                try {
                    Socket socket = listener.accept(); clients.add(socket);
                    Thread worker = new Thread(() -> serve(socket), "bridge-worker-http");
                    worker.setDaemon(true); worker.start();
                } catch (IOException ended) { return; }
            }
        }, "bridge-worker-accept");
        accept.setDaemon(true); accept.start();
    }
    String origin() { return "http://127.0.0.1:" + listener.getLocalPort(); }

    private static String line(BufferedInputStream input) throws IOException {
        StringBuilder result = new StringBuilder();
        for (int value; (value = input.read()) >= 0;) {
            if (value == '\n') return result.toString();
            if (value != '\r') result.append((char) value);
            if (result.length() > 16384) throw new IOException("Header limit");
        }
        return result.length() == 0 ? null : result.toString();
    }
    private void serve(Socket socket) {
        try (Socket client = socket) {
            client.setSoTimeout(10000);
            BufferedInputStream input = new BufferedInputStream(client.getInputStream());
            String request = line(input);
            if (request == null) return;
            String[] fields = request.split(" ", 3);
            if (fields.length != 3) return;
            String method = fields[0], path = fields[1];
            int remaining = 0;
            for (String header; (header = line(input)) != null && !header.isEmpty();) {
                int colon = header.indexOf(':');
                if (colon > 0 && "Content-Length".equalsIgnoreCase(header.substring(0, colon))) {
                    remaining = Integer.parseInt(header.substring(colon + 1).trim());
                }
            }
            if ("POST".equals(method)) posts.incrementAndGet();
            if (path.startsWith("/.gesher-js-bridge/")) reserved.incrementAndGet();
            if (remaining < 0 || remaining > 65536) return;
            byte[] discard = new byte[4096];
            byte[] marker = "fixture-".getBytes(StandardCharsets.US_ASCII);
            int matched = 0;
            while (remaining > 0) {
                int count = input.read(discard, 0, Math.min(discard.length, remaining));
                if (count <= 0) return;
                bodyBytes.addAndGet(count); remaining -= count;
                for (int i = 0; i < count; i++) {
                    if (discard[i] == marker[matched]) {
                        if (++matched == marker.length) { privatePayloadLeaks.incrementAndGet(); matched = 0; }
                    } else { matched = discard[i] == marker[0] ? 1 : 0; }
                }
            }
            String body = null;
            if ("GET".equals(method)) {
                if ("/start".equals(path)) body = page();
                if ("/dedicated.js".equals(path)) body = dedicated();
                if ("/shared.js".equals(path)) body = shared();
                if ("/service.js".equals(path)) body = service();
            }
            boolean found = body != null;
            if (!found) body = "denied";
            byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
            String type = path.endsWith(".js") ? "application/javascript" : "text/html";
            String headers = "HTTP/1.1 " + (found ? "200 OK" : "403 Forbidden")
                    + "\r\nContent-Type: " + type + "; charset=utf-8\r\nContent-Length: " + bytes.length
                    + "\r\nConnection: close\r\nCache-Control: no-store\r\nService-Worker-Allowed: /\r\n\r\n";
            OutputStream output = client.getOutputStream();
            output.write(headers.getBytes(StandardCharsets.US_ASCII)); output.write(bytes); output.flush();
        } catch (Exception disconnected) {
            // A browser may cancel a script/favicon request. Required operations have
            // their own timeouts; no request URL, payload or exception message is logged.
        } finally { clients.remove(socket); }
    }

    private static String fetch() {
        return "async function ask(d){var r=await fetch(d.url,{method:'POST',headers:{'Content-Type':'application/json'},body:d.body});"
                + "return {status:r.status,body:await r.text()};}\n";
    }
    private static String dedicated() {
        return fetch() + "onmessage=async function(e){try{postMessage(await ask(e.data));}catch(x){postMessage({error:true});}};";
    }
    private static String shared() {
        return fetch() + "onconnect=function(e){var p=e.ports[0];p.onmessage=async function(m){"
                + "try{p.postMessage(await ask(m.data));}catch(x){p.postMessage({error:true});}};p.start();};";
    }
    private static String service() {
        return fetch() + "self.addEventListener('install',function(e){e.waitUntil(self.skipWaiting());});"
                + "self.addEventListener('message',function(e){e.waitUntil((async function(){"
                + "try{e.ports[0].postMessage(await ask(e.data));}catch(x){e.ports[0].postMessage({error:true});}})());});";
    }
    private static String page() {
        return "<!doctype html><meta charset=utf-8><title>local bridge worker regression</title><script>\n"
                + "var endpoint=null,phase='bootstrap';var originalOpen=XMLHttpRequest.prototype.open;"
                + "var nativeBridge=chrome.webview.hostObjects.sync.gesherBridge;"
                + "XMLHttpRequest.prototype.open=function(m,u,a){var p=new URL(u,location.href);"
                + "if(p.pathname.indexOf('/.gesher-js-bridge/')===0)endpoint=p.href;return originalOpen.apply(this,arguments);};\n"
                + "function fresh(){var nonce=String(nativeBridge.prepareCall('Fixture','once',JSON.stringify(['worker-fixture-do-not-upload'])));"
                + "if(nonce.length<24||nonce.length>256||nonce.indexOf('fixture-')>=0)throw new Error('bad-nonce');return {url:endpoint,body:nonce};}\n"
                + "function exchange(port,data){return new Promise(function(resolve,reject){var timer=setTimeout(function(){reject(new Error('timeout'));},10000);"
                + "port.onmessage=function(e){clearTimeout(timer);resolve(e.data);};port.onmessageerror=function(){clearTimeout(timer);reject(new Error('message'));};"
                + "if(port.start)port.start();port.postMessage(data);});}\n"
                + "function active(reg){return new Promise(function(resolve,reject){var w=reg.installing||reg.waiting||reg.active;"
                + "if(!w){reject(new Error('missing-worker'));return;}var timer=setTimeout(function(){reject(new Error('activation-timeout'));},10000);"
                + "function done(){if(w.state==='activated'){clearTimeout(timer);resolve(w);}else if(w.state==='redundant'){clearTimeout(timer);reject(new Error('redundant'));}}"
                + "w.addEventListener('statechange',done);done();});}\n"
                + "window.addEventListener('load',async function(){var dedicated=null,shared=null,registration=null;try{"
                + "if(Fixture.bootstrap()!=='ready'||!endpoint)throw new Error('bootstrap');"
                + "phase='dedicated';dedicated=new Worker('/dedicated.js');var dd=fresh(),d;"
                + "try{d=await exchange(dedicated,dd);}finally{nativeBridge.finishCall(dd.body);}"
                + "if(d.status!==200||JSON.parse(d.body).v!=='dedicated-ok'||Fixture.invocations()!==1)throw new Error('dedicated-result');"
                + "dedicated.terminate();dedicated=null;"
                + "phase='shared';if(typeof SharedWorker!=='function')throw new Error('shared-unavailable');"
                + "shared=new SharedWorker('/shared.js','bridge-fixture');var sd=fresh(),s;"
                + "try{s=await exchange(shared.port,sd);}finally{nativeBridge.finishCall(sd.body);}"
                + "if(s.status!==403||Fixture.invocations()!==1)throw new Error('shared-result');shared.port.close();shared=null;"
                + "phase='service';if(!navigator.serviceWorker)throw new Error('service-unavailable');"
                + "registration=await navigator.serviceWorker.register('/service.js',{scope:'/service-scope/'});"
                + "var worker=await active(registration),channel=new MessageChannel();"
                + "var answer=new Promise(function(resolve,reject){var timer=setTimeout(function(){reject(new Error('service-timeout'));},10000);"
                + "channel.port1.onmessage=function(e){clearTimeout(timer);resolve(e.data);};channel.port1.start();});"
                + "var swd=fresh(),sw;worker.postMessage(swd,[channel.port2]);"
                + "try{sw=await answer;}finally{nativeBridge.finishCall(swd.body);channel.port1.close();}"
                + "if(sw.status!==403||Fixture.invocations()!==1)throw new Error('service-result');"
                + "await registration.unregister();registration=null;Fixture.finished();"
                + "}catch(e){try{Fixture.failed(phase);}catch(ignored){console.error('bridgeworkers FAIL '+phase);}}"
                + "finally{if(dedicated)dedicated.terminate();if(shared)shared.port.close();if(registration)registration.unregister();}});\n"
                + "</script><p>Only fixed local fixture values; no external sites.</p>";
    }
    @Override public void close() {
        closed = true;
        try { listener.close(); } catch (IOException ignored) { }
        for (Socket client : clients) try { client.close(); } catch (IOException ignored) { }
    }
}
