// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.io.*;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.net.*;
import java.security.KeyStore;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import javax.net.ssl.*;

/** Host-only TLS and deadline controls complement the shared public wire corpus. */
public final class Http2SessionHost {
    static int checks;
    static void check(boolean value,String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    static void await(Http2SessionContracts.BooleanSupplier value,String message) throws Exception {
        Http2SessionContracts.await(value,message); checks++;
    }
    static HttpEngine engine() { return new HttpEngine.Builder(new android.content.ContextWrapper(null)).build(); }
    static void success(HttpEngine engine,String url,Executor callbacks) throws Exception {
        Http2SessionContracts.Call call=new Http2SessionContracts.Call(engine,url,callbacks);
        call.start(); check(call.await().equals("success") && call.terminals.get()==1,"request result");
    }
    static Throwable tlsFailure(HttpEngine engine,String url,Executor callbacks) throws Exception {
        Http2SessionContracts.Call call=new Http2SessionContracts.Call(engine,url,callbacks);
        call.start();
        try { call.await(); throw new AssertionError("untrusted TLS reused an authenticated connection"); }
        catch (ExecutionException expected) {
            Throwable cause=expected;
            while (cause!=null && !(cause instanceof SSLException)) cause=cause.getCause();
            check(cause!=null,"failure was not an actual TLS rejection");
            check(call.terminals.get()==1,"duplicate failure callback");
            return expected.getCause();
        }
    }
    static SSLContext context(String path) throws Exception {
        KeyStore keys=KeyStore.getInstance("PKCS12");
        try (InputStream input=new FileInputStream(path)) { keys.load(input,"changeit".toCharArray()); }
        KeyManagerFactory km=KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        km.init(keys,"changeit".toCharArray());
        TrustManagerFactory tm=TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        tm.init(keys);
        SSLContext context=SSLContext.getInstance("TLS");
        context.init(km.getKeyManagers(),tm.getTrustManagers(),null);
        return context;
    }
    static SSLServerSocket listener(SSLContext context,String host) throws Exception {
        SSLServerSocket socket=(SSLServerSocket)context.getServerSocketFactory()
                .createServerSocket(0,32,InetAddress.getByName(host));
        SSLParameters parameters=socket.getSSLParameters(); parameters.setApplicationProtocols(new String[]{"h2"});
        socket.setSSLParameters(parameters); return socket;
    }

    /** Two real CONNECT routes; only the declared peer destination is allowed. */
    static final class Tunnel implements Closeable {
        final ServerSocket listener=new ServerSocket(0,8,InetAddress.getByName("127.0.0.1"));
        final Set<Socket> sockets=ConcurrentHashMap.newKeySet();
        final ExecutorService workers=Executors.newCachedThreadPool();
        final AtomicInteger connects=new AtomicInteger();
        final AtomicReference<Throwable> failure=new AtomicReference<>();
        final CountDownLatch connectReceived=new CountDownLatch(1), releaseReply=new CountDownLatch(1);
        final int target;
        volatile boolean closed, holdReply;
        Tunnel(int target) throws IOException {
            this.target=target;
            workers.execute(() -> {
                while (!closed) try {
                    Socket client=listener.accept(); sockets.add(client); workers.execute(() -> serve(client));
                } catch (Exception error) { if (!closed) failure.compareAndSet(null,error); }
            });
        }
        Proxy proxy() { return new Proxy(Proxy.Type.HTTP,InetSocketAddress.createUnresolved("localhost",listener.getLocalPort())); }
        void serve(Socket client) {
            Socket remote=null;
            try {
                client.setSoTimeout(10000); client.setTcpNoDelay(true);
                ByteArrayOutputStream bytes=new ByteArrayOutputStream(); InputStream in=client.getInputStream();
                int tail=0;
                while (tail!=0x0d0a0d0a) {
                    int value=in.read(); if (value<0) throw new EOFException();
                    bytes.write(value); tail=(tail<<8)|value;
                    if (bytes.size()>8192) throw new IOException("oversized CONNECT");
                }
                String header=bytes.toString("US-ASCII");
                if (!header.startsWith("CONNECT 127.0.0.1:"+target+" HTTP/1.1\r\n")) {
                    throw new IOException("unexpected CONNECT destination");
                }
                connects.incrementAndGet();
                connectReceived.countDown();
                if (holdReply && !releaseReply.await(10,TimeUnit.SECONDS)) throw new IOException("held CONNECT deadline");
                if (closed) return;
                remote=new Socket("127.0.0.1",target); remote.setTcpNoDelay(true); sockets.add(remote);
                client.getOutputStream().write("HTTP/1.1 200 Connection established\r\n\r\n".getBytes("US-ASCII"));
                client.getOutputStream().flush();
                final Socket upstream=remote;
                workers.execute(() -> copy(upstream,client));
                copy(client,remote);
            } catch (EOFException | SocketException retired) { }
            catch (Exception error) { if (!closed) failure.compareAndSet(null,error); }
            finally { retire(client); if (remote!=null) retire(remote); }
        }
        void copy(Socket from,Socket to) {
            try { from.getInputStream().transferTo(to.getOutputStream()); }
            catch (IOException ended) { }
            finally { retire(from); retire(to); }
        }
        void retire(Socket socket) {
            sockets.remove(socket); try { socket.close(); } catch (IOException ignored) { }
        }
        @Override public void close() throws IOException {
            closed=true; releaseReply.countDown(); listener.close(); for (Socket socket:sockets) retire(socket); workers.shutdownNow();
            if (failure.get()!=null) throw new AssertionError("CONNECT peer failed",failure.get());
        }
    }

    static void tls(String path,Executor callbacks,boolean budgets) throws Exception {
        SSLSocketFactory saved=HttpsURLConnection.getDefaultSSLSocketFactory();
        ProxySelector selector=ProxySelector.getDefault();
        SSLContext trusted=context(path);
        HttpsURLConnection.setDefaultSSLSocketFactory(trusted.getSocketFactory());
        HttpEngine first=engine(),second=engine();
        try (Http2SessionContracts.Peer peer=new Http2SessionContracts.Peer(listener(trusted,"127.0.0.1"))) {
            int count=32; long begin=System.nanoTime();
            for (int i=0;i<count;i++) success(first,peer.url(),callbacks);
            long elapsed=System.nanoTime()-begin;
            System.out.println("HTTP2_TLS_SESSIONS requests="+count+" connections="+peer.accepted.get()
                    +" handshakes="+peer.handshakes.get()+" bodies="+(count*4093)+" elapsed_ns="+elapsed);
            if (!budgets) return;
            check(peer.accepted.get()==1 && peer.handshakes.get()==1,"TLS handshakes not reused");
            success(second,peer.url(),callbacks);
            check(peer.accepted.get()==2,"engines shared a TLS session");
            second.shutdown();
            success(first,peer.url(),callbacks);
            check(peer.accepted.get()==2,"other engine shutdown retired a live owner");
            success(first,peer.url().replace("127.0.0.1","localhost"),callbacks);
            check(peer.accepted.get()==3,"different origins were coalesced");
            SSLContext replacement=context(path);
            SSLSocketFactory replacementFactory=replacement.getSocketFactory();
            HttpsURLConnection.setDefaultSSLSocketFactory(replacementFactory);
            success(first,peer.url(),callbacks);
            check(peer.accepted.get()==4,"different TLS factories shared a session");
            SSLContext untrusted=SSLContext.getInstance("TLS"); untrusted.init(null,null,null);
            HttpsURLConnection.setDefaultSSLSocketFactory(untrusted.getSocketFactory());
            peer.expectHandshakeFailure=true;
            tlsFailure(first,peer.url(),callbacks);
            await(() -> peer.rejectedHandshakes.get()==1,"server did not observe trust rejection");
            HttpsURLConnection.setDefaultSSLSocketFactory(replacementFactory);
            success(first,peer.url(),callbacks);
            check(peer.accepted.get()==5,"failed trust probe poisoned an unrelated authenticated session");
            try (Tunnel a=new Tunnel(peer.server.getLocalPort()); Tunnel b=new Tunnel(peer.server.getLocalPort())) {
                AtomicReference<Proxy> route=new AtomicReference<>(a.proxy());
                ProxySelector.setDefault(new ProxySelector() {
                    public List<Proxy> select(URI uri) { return Collections.singletonList(route.get()); }
                    public void connectFailed(URI uri,SocketAddress address,IOException failure) { }
                });
                success(first,peer.url(),callbacks); success(first,peer.url(),callbacks);
                check(a.connects.get()==1,"same CONNECT route not reused");
                route.set(b.proxy()); success(first,peer.url(),callbacks);
                check(b.connects.get()==1 && a.connects.get()==1,"proxy routes were crossed");
                route.set(a.proxy()); success(first,peer.url(),callbacks);
                check(a.connects.get()==1,"returning CONNECT route not reused");
                ProxySelector.setDefault(selector);
                first.shutdown();
                await(() -> peer.open.get()==0,"TLS shutdown retained a physical connection");
            }
            peer.healthy();
            try (Http2SessionContracts.Peer wrongHost=new Http2SessionContracts.Peer(listener(trusted,"127.0.0.2"))) {
                wrongHost.expectHandshakeFailure=true;
                HttpEngine engine=engine();
                try {
                    Throwable failure=tlsFailure(engine,wrongHost.url(),callbacks);
                    while (failure!=null && !(failure instanceof java.security.cert.CertificateException)) {
                        failure=failure.getCause();
                    }
                    check(failure!=null,"hostname failure did not reject the actual certificate");
                }
                finally { engine.shutdown(); }
                await(() -> wrongHost.open.get()==0,"hostname rejection retained its socket");
                check(wrongHost.accepted.get()==1 && wrongHost.requests.get()==0
                        && wrongHost.handshakes.get()==0,"invalid hostname reached an authenticated HTTP session");
                wrongHost.healthy();
            }
        } finally {
            first.shutdown(); second.shutdown();
            HttpsURLConnection.setDefaultSSLSocketFactory(saved); ProxySelector.setDefault(selector);
        }
    }

    /** Short expiry and idle bounds are isolated controls, never production defaults. */
    static void configureIdle(HttpEngine engine,int maxIdle,long nanos) throws Exception {
        Field field=engine.getClass().getDeclaredField("http2Sessions"); field.setAccessible(true);
        Constructor<?> constructor=field.getType().getDeclaredConstructor(int.class,long.class); constructor.setAccessible(true);
        ((Closeable)field.get(engine)).close(); field.set(engine,constructor.newInstance(maxIdle,nanos));
    }
    static void idle(Executor callbacks) throws Exception {
        HttpEngine engine=engine(); configureIdle(engine,2,TimeUnit.SECONDS.toNanos(10));
        try (Http2SessionContracts.Peer a=new Http2SessionContracts.Peer();
             Http2SessionContracts.Peer b=new Http2SessionContracts.Peer();
             Http2SessionContracts.Peer c=new Http2SessionContracts.Peer()) {
            success(engine,a.url(),callbacks); success(engine,b.url(),callbacks); success(engine,c.url(),callbacks);
            await(() -> a.open.get()==0 && b.open.get()==1 && c.open.get()==1,"idle owner bound/LRU retirement");
            engine.shutdown();
            await(() -> b.open.get()==0 && c.open.get()==0,"bounded pool shutdown");
        } finally { engine.shutdown(); }
        engine=engine(); configureIdle(engine,2,TimeUnit.MILLISECONDS.toNanos(100));
        try (Http2SessionContracts.Peer peer=new Http2SessionContracts.Peer()) {
            success(engine,peer.url(),callbacks);
            await(() -> peer.open.get()==0,"idle deadline did not close real socket");
            success(engine,peer.url(),callbacks);
            check(peer.accepted.get()==2,"expired session reused");
            engine.shutdown(); await(() -> peer.open.get()==0,"idle cleanup after shutdown");
            peer.healthy();
        } finally { engine.shutdown(); }
    }
    static void networks(Executor callbacks) throws Exception {
        // These host Network objects delegate to actual loopback DNS/sockets;
        // they do not claim host routing equivalence to Android netd.
        android.net.Network a=new android.net.Network(101), b=new android.net.Network(102);
        android.net.Network alias=new android.net.Network(101);
        HttpEngine engine=engine();
        try (Http2SessionContracts.Peer peer=new Http2SessionContracts.Peer()) {
            engine.bindToNetwork(a);
            Http2SessionContracts.Call captured=new Http2SessionContracts.Call(engine,peer.url(),callbacks);
            engine.bindToNetwork(b); captured.start();
            check(captured.await().equals("success"),"builder-captured network result");
            check(a.sockets.get()==1 && a.resolutions.get()==1 && b.sockets.get()==0,
                    "builder did not retain its selected network");
            success(engine,peer.url(),callbacks); success(engine,peer.url(),callbacks);
            check(b.sockets.get()==1 && b.resolutions.get()==1 && peer.accepted.get()==2,
                    "different network route reused or repeated DNS/socket work");
            engine.bindToNetwork(a); success(engine,peer.url(),callbacks);
            check(a.sockets.get()==1 && peer.accepted.get()==2,"same explicit owner not reused");
            engine.bindToNetwork(alias); success(engine,peer.url(),callbacks);
            check(a.equals(alias) && alias.sockets.get()==1 && peer.accepted.get()==3,
                    "equal network identifiers shared different supplied owners");
            engine.bindToNetwork(null); success(engine,peer.url(),callbacks);
            check(peer.accepted.get()==4,"default route reused an explicit network");
            engine.shutdown(); await(() -> peer.open.get()==0,"network-owned sockets did not retire");
            peer.healthy();
        } finally { engine.shutdown(); }
    }
    static void connectionFailures(String path,Executor callbacks) throws Exception {
        SSLContext trusted=context(path);
        SSLSocketFactory saved=HttpsURLConnection.getDefaultSSLSocketFactory();
        ProxySelector selector=ProxySelector.getDefault();
        HttpsURLConnection.setDefaultSSLSocketFactory(trusted.getSocketFactory());
        try {
            // A real successful TLS handshake without ALPN must not be reused
            // as an HTTP/2 session or emit an HTTP/2 preface.
            try (SSLServerSocket server=listener(trusted,"127.0.0.1")) {
                SSLParameters parameters=server.getSSLParameters();
                parameters.setApplicationProtocols(new String[0]); server.setSSLParameters(parameters);
                CompletableFuture<Boolean> noHttp=new CompletableFuture<>();
                Thread peer=new Thread(() -> {
                    try (SSLSocket socket=(SSLSocket)server.accept()) {
                        socket.setSoTimeout(5000); socket.startHandshake();
                        noHttp.complete(socket.getApplicationProtocol().isEmpty() && socket.getInputStream().read()==-1);
                    } catch (Throwable failure) { noHttp.completeExceptionally(failure); }
                },"http2-no-alpn-peer");
                peer.setDaemon(true); peer.start();
                HttpEngine engine=engine();
                try { tlsFailure(engine,"https://127.0.0.1:"+server.getLocalPort()+"/",callbacks); }
                finally { engine.shutdown(); }
                check(noHttp.get(5,TimeUnit.SECONDS),"TLS without h2 emitted an HTTP preface or retained socket");
            }
            try (Http2SessionContracts.Peer peer=new Http2SessionContracts.Peer(listener(trusted,"127.0.0.1"));
                 Tunnel tunnel=new Tunnel(peer.server.getLocalPort())) {
                tunnel.holdReply=true;
                ProxySelector.setDefault(new ProxySelector() {
                    public List<Proxy> select(URI uri) { return Collections.singletonList(tunnel.proxy()); }
                    public void connectFailed(URI uri,SocketAddress address,IOException failure) { }
                });
                HttpEngine engine=engine();
                try {
                    Http2SessionContracts.Call call=new Http2SessionContracts.Call(engine,peer.url(),callbacks);
                    call.start(); check(tunnel.connectReceived.await(5,TimeUnit.SECONDS),"CONNECT was not admitted");
                    call.stream.cancel();
                    check(call.await().equals("canceled") && call.terminals.get()==1,"connecting cancellation did not retire worker");
                    engine.shutdown();
                    check(peer.accepted.get()==0 && peer.requests.get()==0,"canceled CONNECT emitted TLS or HTTP");
                } finally { engine.shutdown(); }
            }
        } finally {
            HttpsURLConnection.setDefaultSSLSocketFactory(saved); ProxySelector.setDefault(selector);
        }
    }
    static boolean blockedHeaderWrite() {
        for (StackTraceElement[] stack:Thread.getAllStackTraces().values()) {
            boolean admission=false, socketWrite=false;
            for (StackTraceElement frame:stack) {
                admission|=frame.getClassName().equals("android.net.http.Http2SessionPool$Session")
                        && frame.getMethodName().equals("start");
                socketWrite|=(frame.getClassName().equals("sun.nio.ch.NioSocketImpl")
                        && frame.getMethodName().equals("implWrite"))
                        || (frame.getClassName().equals("java.net.Socket$SocketOutputStream")
                        && frame.getMethodName().equals("write"));
            }
            if (admission && socketWrite) return true;
        }
        return false;
    }
    static void physicalWriteDeadline(Executor callbacks) throws Exception {
        HttpEngine engine=engine();
        android.net.Network network=new android.net.Network(103); network.sendBufferBytes=1024;
        engine.bindToNetwork(network);
        CountDownLatch releasePeer=new CountDownLatch(1), ready=new CountDownLatch(1);
        CompletableFuture<String> terminal=new CompletableFuture<>();
        CompletableFuture<Integer> retired=new CompletableFuture<>();
        AtomicInteger terminals=new AtomicInteger();
        try (ServerSocket server=new ServerSocket()) {
            server.setReceiveBufferSize(1024); server.bind(new InetSocketAddress("127.0.0.1",0),1);
            Thread peer=new Thread(() -> {
                int received=0;
                try (Socket socket=server.accept()) {
                    socket.setSoTimeout(5000);
                    if (!releasePeer.await(15,TimeUnit.SECONDS)) throw new AssertionError("write deadline did not retire owner");
                    byte[] bytes=new byte[4096];
                    for (int n;(n=socket.getInputStream().read(bytes))!=-1;) received+=n;
                    retired.complete(received);
                } catch (SocketException closed) { retired.complete(received); }
                catch (Throwable error) { retired.completeExceptionally(error); }
            },"http2-unread-connection-peer");
            peer.setDaemon(true); peer.start();
            byte[] input=new byte[1024*1024]; new Random(103).nextBytes(input);
            String value=Base64.getEncoder().encodeToString(input);
            BidirectionalStream stream=engine.newBidirectionalStreamBuilder(
                    "http://127.0.0.1:"+server.getLocalPort()+"/unread",callbacks,new BidirectionalStream.Callback() {
                        public void onStreamReady(BidirectionalStream stream) { ready.countDown(); }
                        public void onResponseHeadersReceived(BidirectionalStream stream,UrlResponseInfo info) {
                            terminal.completeExceptionally(new AssertionError("non-reading peer sent headers"));
                        }
                        public void onReadCompleted(BidirectionalStream stream,UrlResponseInfo info,java.nio.ByteBuffer b,boolean end) { }
                        public void onWriteCompleted(BidirectionalStream stream,UrlResponseInfo info,java.nio.ByteBuffer b,boolean end) { }
                        public void onResponseTrailersReceived(BidirectionalStream stream,UrlResponseInfo info,HeaderBlock trailers) { }
                        public void onSucceeded(BidirectionalStream stream,UrlResponseInfo info) {
                            terminals.incrementAndGet(); terminal.complete("success");
                        }
                        public void onCanceled(BidirectionalStream stream,UrlResponseInfo info) {
                            terminals.incrementAndGet(); terminal.complete("canceled");
                        }
                        public void onFailed(BidirectionalStream stream,UrlResponseInfo info,HttpException error) {
                            terminals.incrementAndGet(); terminal.completeExceptionally(error);
                        }
                    }).setHttpMethod("GET").addHeader("x-owned-large-header",value)
                    .setDelayRequestHeadersUntilFirstFlushEnabled(true).build();
            try {
                stream.start(); check(ready.await(5,TimeUnit.SECONDS),"unread peer connection ready");
                Field poolField=engine.getClass().getDeclaredField("http2Sessions"); poolField.setAccessible(true);
                Object pool=poolField.get(engine);
                Field sessions=pool.getClass().getDeclaredField("sessions"); sessions.setAccessible(true);
                Object session=((List<?>)sessions.get(pool)).get(0);
                Field connectionField=session.getClass().getDeclaredField("connection"); connectionField.setAccessible(true);
                Object connection=connectionField.get(session);
                Field writerField=connection.getClass().getDeclaredField("frameWriter"); writerField.setAccessible(true);
                Object writer=writerField.get(connection);
                Field sinkField=writer.getClass().getDeclaredField("sink"); sinkField.setAccessible(true);
                Object sink=sinkField.get(writer);
                Class<?> sinkType=Class.forName("com.android.okhttp.okio.Sink"), timeoutType=Class.forName("com.android.okhttp.okio.Timeout");
                Object timeout=sinkType.getMethod("timeout").invoke(sink);
                check((Long)timeoutType.getMethod("timeoutNanos").invoke(timeout)==TimeUnit.SECONDS.toNanos(30),
                        "physical write deadline missing");
                // Isolated host deadline control; production remains 30 seconds.
                timeoutType.getMethod("timeout",long.class,TimeUnit.class).invoke(timeout,5L,TimeUnit.SECONDS);
                stream.flush(); await(Http2SessionHost::blockedHeaderWrite,"large HEADERS did not block on actual socket write");
                stream.cancel();
                check(terminal.get(8,TimeUnit.SECONDS).equals("canceled") && terminals.get()==1,
                        "physical write deadline did not retire canceled header worker");
                engine.shutdown(); releasePeer.countDown();
                check(retired.get(5,TimeUnit.SECONDS)<value.length(),"peer consumed the complete header fixture");
            } finally { stream.cancel(); releasePeer.countDown(); }
        } finally { engine.shutdown(); }
    }
    static int admissionWaiters() {
        int count=0;
        for (Map.Entry<Thread,StackTraceElement[]> thread:Thread.getAllStackTraces().entrySet()) {
            if (thread.getKey().getState()!=Thread.State.TIMED_WAITING) continue;
            for (StackTraceElement frame:thread.getValue()) if (frame.getClassName().equals("android.net.http.Http2SessionPool$Session")
                    && frame.getMethodName().equals("start")) { count++; break; }
        }
        return count;
    }
    static void settings(Executor callbacks) throws Exception {
        HttpEngine engine=engine();
        try (Http2SessionContracts.Peer peer=new Http2SessionContracts.Peer()) {
            peer.maximumStreams=0;
            Http2SessionContracts.Call first=new Http2SessionContracts.Call(engine,peer.url(),callbacks,true);
            Http2SessionContracts.Call canceled=new Http2SessionContracts.Call(engine,peer.url(),callbacks,true);
            first.start(); check(first.ready.await(5,TimeUnit.SECONDS),"first ready");
            await(() -> peer.settingsAcks.get()==1,"first SETTINGS not applied"); first.stream.flush();
            canceled.start(); check(canceled.ready.await(5,TimeUnit.SECONDS),"second ready");
            await(() -> peer.settingsAcks.get()==2,"second SETTINGS not applied"); canceled.stream.flush();
            await(() -> admissionWaiters()>=2,"zero-stream setting did not block admission");
            canceled.stream.cancel(); check(canceled.await().equals("canceled"),"cancel waiting for SETTINGS");
            check(peer.requests.get()==0 && peer.resets.get()==0,"zero-stream setting was bypassed");
            peer.maximumStreams(1);
            check(first.await().equals("success"),"SETTINGS increase did not resume admission");
            check(peer.requests.get()==1,"canceled admission emitted headers");
            int priorAcks=peer.settingsAcks.get(); peer.maximumStreams(-1);
            await(() -> peer.settingsAcks.get()>=priorAcks+peer.open.get(),"unsigned SETTINGS not applied");
            int priorConnections=peer.accepted.get();
            success(engine,peer.url(),callbacks);
            check(peer.accepted.get()==priorConnections,"uint32 maximum was treated as a negative limit");
            engine.shutdown(); await(() -> peer.open.get()==0,"SETTINGS waiter ownership leaked"); peer.healthy();
        } finally { engine.shutdown(); }
    }
    public static void main(String[] args) throws Exception {
        boolean budgets=args.length<2 || !args[1].equals("baseline");
        ExecutorService callbacks=Executors.newFixedThreadPool(4);
        try {
            tls(args[0],callbacks,budgets);
            if (budgets) {
                idle(callbacks); settings(callbacks); networks(callbacks);
                connectionFailures(args[0],callbacks); physicalWriteDeadline(callbacks);
            }
        } finally { callbacks.shutdownNow(); }
        System.out.println("Http2SessionHost passed: "+checks+" actual TLS/proxy/ownership/deadline checks; budgets="+budgets);
    }
}
