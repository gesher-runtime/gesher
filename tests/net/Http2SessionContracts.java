// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.io.*;
import java.net.*;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import javax.net.ssl.*;

/** Independent wire peer and public API corpus, shared by host JVM and ART. */
public final class Http2SessionContracts {
    static final byte[] BODY = new byte[4093];
    static { new Random(91).nextBytes(BODY); }
    private static int checks;
    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    static void await(BooleanSupplier condition, String message) throws Exception {
        long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
        while (!condition.get() && System.nanoTime()<deadline) Thread.sleep(5);
        check(condition.get(),message);
    }
    interface BooleanSupplier { boolean get(); }

    static final class Peer implements Closeable {
        final ServerSocket server;
        final AtomicInteger accepted=new AtomicInteger(), open=new AtomicInteger(), requests=new AtomicInteger(), resets=new AtomicInteger();
        final AtomicInteger handshakes=new AtomicInteger(), rejectedHandshakes=new AtomicInteger(), settingsAcks=new AtomicInteger();
        final List<Client> clients=Collections.synchronizedList(new ArrayList<>());
        final AtomicReference<Throwable> failure=new AtomicReference<>();
        volatile boolean hold, closing;
        /** The next admitted stream receives its full advertised window, unread. */
        volatile boolean flood;
        final AtomicLong flooded=new AtomicLong();
        volatile boolean expectHandshakeFailure;
        volatile int maximumStreams=Integer.MAX_VALUE;
        final Thread acceptor;
        Peer() throws IOException {
            this(new ServerSocket(0,64,InetAddress.getByName("127.0.0.1")));
        }
        Peer(ServerSocket server) throws IOException {
            this.server=server;
            acceptor=new Thread(() -> {
                while (!closing) try {
                    Socket socket=server.accept();
                    accepted.incrementAndGet(); open.incrementAndGet();
                    Client client=new Client(socket); clients.add(client);
                    Thread worker=new Thread(client,"http2-session-peer");
                    worker.setDaemon(true); worker.start();
                } catch (Throwable error) { if (!closing) failure.compareAndSet(null,error); }
            },"http2-session-accept");
            acceptor.setDaemon(true); acceptor.start();
        }
        String url() { return (server instanceof SSLServerSocket ? "https" : "http")
                +"://"+server.getInetAddress().getHostAddress()+":"+server.getLocalPort()+"/sessions"; }
        void healthy() { if (failure.get()!=null) throw new AssertionError("wire peer failed",failure.get()); }
        void finishAll() throws IOException {
            hold=false;
            synchronized (clients) { for (Client client:clients) client.finishAll(); }
        }
        void goAway() throws IOException {
            synchronized (clients) { for (Client client:clients) client.goAway(); }
        }
        void resetOne() throws IOException {
            synchronized (clients) { for (Client client:clients) synchronized (client) {
                if (!client.pending.isEmpty()) {
                    int id=client.pending.iterator().next(); client.pending.remove(id);
                    client.send(3,0,id,new byte[]{0,0,0,8}); return;
                }
            }}
            throw new AssertionError("no admitted stream to reset");
        }
        void maximumStreams(int value) throws IOException {
            maximumStreams=value;
            synchronized (clients) { for (Client client:clients) if (!client.socket.isClosed()) client.settings(); }
        }
        @Override public void close() throws IOException {
            closing=true; server.close();
            synchronized (clients) { for (Client client:clients) client.socket.close(); }
        }
        final class Client implements Runnable {
            final Socket socket;
            final OutputStream out;
            final Set<Integer> pending=new LinkedHashSet<>();
            // Honor the client's actual receive windows (RFC 9113 6.9). A body
            // without connection credit waits for WINDOW_UPDATE, as a real
            // server must, so a stalled session is observable.
            final Set<Integer> starved=new LinkedHashSet<>();
            long streamWindow=65535, connectionCredit=65535;
            int last;
            Client(Socket socket) throws IOException { this.socket=socket; out=socket.getOutputStream(); }
            synchronized void send(int type,int flags,int stream,byte[] bytes) throws IOException {
                Http2DeviceSmoke.frame(out,type,flags,stream,bytes);
            }
            synchronized void finish(int id) throws IOException {
                if (!pending.contains(id)) return;
                if (connectionCredit<BODY.length) { starved.add(id); return; }
                pending.remove(id); starved.remove(id);
                connectionCredit-=BODY.length;
                send(0,1,id,BODY);
            }
            synchronized void floodStream(int id) throws IOException {
                long remaining=Math.min(streamWindow,connectionCredit);
                byte[] chunk=new byte[16384];
                while (remaining>0) {
                    int n=(int)Math.min(chunk.length,remaining);
                    send(0,0,id,n==chunk.length ? chunk : Arrays.copyOf(chunk,n));
                    remaining-=n; connectionCredit-=n; flooded.addAndGet(n);
                }
            }
            synchronized void finishAll() throws IOException {
                for (int id:new ArrayList<>(pending)) finish(id);
            }
            synchronized void goAway() throws IOException {
                if (socket.isClosed()) return;
                byte[] bytes=new byte[8];
                bytes[0]=(byte)(last>>24); bytes[1]=(byte)(last>>16);
                bytes[2]=(byte)(last>>8); bytes[3]=(byte)last;
                send(7,0,0,bytes);
            }
            synchronized void settings() throws IOException {
                int n=maximumStreams;
                send(4,0,0,new byte[]{0,3,(byte)(n>>24),(byte)(n>>16),(byte)(n>>8),(byte)n});
            }
            public void run() {
                try (Socket ignored=socket) {
                    socket.setTcpNoDelay(true);
                    socket.setSoTimeout(15000);
                    if (socket instanceof SSLSocket) {
                        SSLSocket ssl=(SSLSocket)socket; ssl.startHandshake();
                        if (!ssl.getApplicationProtocol().equals("h2")) throw new IOException("wrong ALPN");
                        handshakes.incrementAndGet();
                    }
                    DataInputStream in=new DataInputStream(socket.getInputStream());
                    byte[] preface=new byte[24]; in.readFully(preface);
                    if (!new String(preface,java.nio.charset.StandardCharsets.US_ASCII)
                            .equals("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")) throw new IOException("bad preface");
                    settings();
                    for (;;) {
                        int size=(in.readUnsignedByte()<<16)|(in.readUnsignedByte()<<8)|in.readUnsignedByte();
                        int type=in.readUnsignedByte(),flags=in.readUnsignedByte(),id=in.readInt()&0x7fffffff;
                        if (size>65536) throw new IOException("unexpected frame size");
                        byte[] bytes=new byte[size]; in.readFully(bytes);
                        if (type==4 && (flags&1)==0) synchronized (this) {
                            for (int i=0;i+6<=bytes.length;i+=6) {
                                int key=((bytes[i]&255)<<8)|(bytes[i+1]&255);
                                long value=((bytes[i+2]&255L)<<24)|((bytes[i+3]&255)<<16)|((bytes[i+4]&255)<<8)|(bytes[i+5]&255);
                                if (key==4) streamWindow=value;
                            }
                            send(4,1,0,new byte[0]);
                        }
                        if (type==8 && id==0 && bytes.length==4) synchronized (this) {
                            connectionCredit+=((bytes[0]&127L)<<24)|((bytes[1]&255)<<16)|((bytes[2]&255)<<8)|(bytes[3]&255);
                            for (int starving:new ArrayList<>(starved)) finish(starving);
                        }
                        if (type==4 && (flags&1)!=0) settingsAcks.incrementAndGet();
                        if (type==6 && (flags&1)==0) send(6,1,0,bytes);
                        if (type==1) synchronized (this) {
                            if (id<=last || (id&1)==0 || (flags&4)==0) throw new IOException("stream identity/header order");
                            last=id; pending.add(id); requests.incrementAndGet();
                            send(1,4,id,new byte[]{(byte)0x88});
                            if (flood) { flood=false; floodStream(id); }
                            else if (!hold) finish(id);
                        }
                        if (type==3) synchronized (this) {
                            if (bytes.length!=4) throw new IOException("bad RST_STREAM");
                            pending.remove(id); starved.remove(id); resets.incrementAndGet();
                        }
                    }
                } catch (SSLHandshakeException rejected) {
                    rejectedHandshakes.incrementAndGet();
                    if (!expectHandshakeFailure) failure.compareAndSet(null,rejected);
                } catch (EOFException | SocketException ended) {
                    // A baseline private session or an engine shutdown owns EOF.
                } catch (Throwable error) { if (!closing) failure.compareAndSet(null,error); }
                finally {
                    synchronized (this) { pending.clear(); starved.clear(); }
                    open.decrementAndGet();
                }
            }
        }
    }

    static final class Call implements BidirectionalStream.Callback {
        final CompletableFuture<String> terminal=new CompletableFuture<>();
        final CountDownLatch headers=new CountDownLatch(1);
        final CountDownLatch ready=new CountDownLatch(1);
        final AtomicInteger terminals=new AtomicInteger();
        final ByteBuffer buffer=ByteBuffer.allocateDirect(777);
        final ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        final BidirectionalStream stream;
        final byte[] upload;
        volatile boolean reading=true;
        Call(HttpEngine engine, String url, Executor executor) {
            this(engine,url,executor,false);
        }
        Call(HttpEngine engine, String url, Executor executor, boolean delay) {
            this(engine,url,executor,delay,null);
        }
        /** A non-null upload is sent as one POST body ending the request stream. */
        Call(HttpEngine engine, String url, Executor executor, boolean delay, byte[] upload) {
            this.upload=upload;
            stream=engine.newBidirectionalStreamBuilder(url,executor,this).setHttpMethod(upload==null ? "GET" : "POST")
                    .setDelayRequestHeadersUntilFirstFlushEnabled(delay).build();
        }
        void start() { stream.start(); }
        String await() throws Exception { return terminal.get(8,TimeUnit.SECONDS); }
        public void onStreamReady(BidirectionalStream stream) {
            ready.countDown();
            if (upload!=null) {
                ByteBuffer body=ByteBuffer.allocateDirect(upload.length); body.put(upload).flip();
                stream.write(body,true); stream.flush();
            }
        }
        public void onResponseHeadersReceived(BidirectionalStream stream,UrlResponseInfo info) {
            if (info.getHttpStatusCode()!=200 || !info.getNegotiatedProtocol().equals("h2")) {
                throw new AssertionError("response metadata");
            }
            headers.countDown(); if (reading) stream.read(buffer);
        }
        public void onReadCompleted(BidirectionalStream stream,UrlResponseInfo info,ByteBuffer buffer,boolean end) {
            buffer.flip(); while (buffer.hasRemaining()) bytes.write(buffer.get()&255); buffer.clear();
            if (!end) stream.read(buffer);
        }
        public void onWriteCompleted(BidirectionalStream stream,UrlResponseInfo info,ByteBuffer buffer,boolean end) { }
        public void onResponseTrailersReceived(BidirectionalStream stream,UrlResponseInfo info,HeaderBlock trailers) { }
        public void onSucceeded(BidirectionalStream stream,UrlResponseInfo info) {
            terminals.incrementAndGet();
            if (!Arrays.equals(BODY,bytes.toByteArray()) || info.getReceivedByteCount()!=BODY.length) {
                terminal.completeExceptionally(new AssertionError("stream body/count isolation"));
            } else terminal.complete("success");
        }
        public void onCanceled(BidirectionalStream stream,UrlResponseInfo info) {
            terminals.incrementAndGet(); terminal.complete("canceled");
        }
        public void onFailed(BidirectionalStream stream,UrlResponseInfo info,HttpException error) {
            terminals.incrementAndGet(); terminal.completeExceptionally(error);
        }
    }

    static boolean sessionsRetired(HttpEngine engine) {
        try {
            java.lang.reflect.Field poolField=engine.getClass().getDeclaredField("http2Sessions");
            poolField.setAccessible(true); Object pool=poolField.get(engine);
            java.lang.reflect.Field sessions=pool.getClass().getDeclaredField("sessions"); sessions.setAccessible(true);
            synchronized (pool) {
                for (Object session:(List<?>)sessions.get(pool)) {
                    java.lang.reflect.Method healthy=session.getClass().getDeclaredMethod("healthy"); healthy.setAccessible(true);
                    if ((Boolean)healthy.invoke(session)) return false;
                }
                return true;
            }
        } catch (Exception failure) { throw new RuntimeException(failure); }
    }
    static HttpEngine engine() { return new HttpEngine.Builder(new android.content.ContextWrapper(null)).build(); }
    static void retire(HttpEngine engine,Peer peer,List<Call> owned) throws Exception {
        for (Call call:owned) call.stream.cancel();
        for (Call call:owned) { try { call.await(); } catch (ExecutionException expectedFailure) { } }
        engine.shutdown();
        await(() -> peer.open.get()==0,"engine shutdown retained owned socket(s)");
        peer.healthy();
    }

    /** A completed request body must not end its stream with RST_STREAM. */
    static void completedUploads(ExecutorService callbacks) throws Exception {
        try (Peer peer=new Peer()) {
            HttpEngine engine=engine(); List<Call> owned=new ArrayList<>();
            try {
                byte[] upload=new byte[1531]; new Random(17).nextBytes(upload);
                for (int i=0;i<8;i++) {
                    Call call=new Call(engine,peer.url(),callbacks,false,upload); owned.add(call); call.start();
                    check(call.await().equals("success") && call.terminals.get()==1,"POST body terminal/result");
                }
                Call after=new Call(engine,peer.url(),callbacks); owned.add(after); after.start();
                check(after.await().equals("success"),"request after uploads on the shared session");
                // A reset would be queued before the terminal callback; allow its
                // asynchronous provider writer to reach the peer before checking.
                Thread.sleep(300);
                check(peer.accepted.get()==1 && peer.requests.get()==9,"uploads did not share one session");
                check(peer.resets.get()==0,"completed upload stream was reset ("+peer.resets.get()+" RST_STREAM)");
            } finally { retire(engine,peer,owned); }
        }
    }

    /** One stream whose application stopped reading leaves credit for others. */
    static void pausedReader(ExecutorService callbacks) throws Exception {
        try (Peer peer=new Peer()) {
            HttpEngine engine=engine(); List<Call> owned=new ArrayList<>();
            try {
                peer.flood=true;
                Call paused=new Call(engine,peer.url(),callbacks); paused.reading=false; owned.add(paused); paused.start();
                check(paused.headers.await(5,TimeUnit.SECONDS),"paused stream headers");
                await(() -> !peer.flood && peer.flooded.get()>0,"peer did not fill the paused stream window");
                Call sibling=new Call(engine,peer.url(),callbacks); owned.add(sibling); sibling.start();
                try { check(sibling.await().equals("success"),"sibling stream result"); }
                catch (TimeoutException stalled) {
                    throw new AssertionError("paused stream ("+peer.flooded.get()+" unread bytes) stalled its session sibling");
                }
                check(peer.accepted.get()==1,"sibling did not share the paused stream's session");
                paused.stream.cancel(); check(paused.await().equals("canceled"),"paused stream cancellation");
            } finally { retire(engine,peer,owned); }
        }
    }

    /** GOAWAY after session selection but before HEADERS leaves the request unsent. */
    static void goAwayBeforeHeaders(ExecutorService callbacks) throws Exception {
        try (Peer peer=new Peer()) {
            HttpEngine engine=engine(); List<Call> owned=new ArrayList<>();
            try {
                Call warm=new Call(engine,peer.url(),callbacks); owned.add(warm); warm.start();
                check(warm.await().equals("success"),"warm session request");
                Call delayed=new Call(engine,peer.url(),callbacks,true); owned.add(delayed); delayed.start();
                check(delayed.ready.await(5,TimeUnit.SECONDS),"delayed stream ready on the warm session");
                check(peer.accepted.get()==1,"delayed stream did not select the warm session");
                peer.goAway();
                await(() -> sessionsRetired(engine),"GOAWAY did not retire session admission");
                delayed.stream.flush();
                check(delayed.await().equals("success") && delayed.terminals.get()==1,
                        "request selected before GOAWAY was not admitted on a fresh session");
                check(peer.accepted.get()==2 && peer.requests.get()==2,"retry was not one fresh unsent admission");
                await(() -> peer.open.get()==1,"retired GOAWAY session was not released");
            } finally { retire(engine,peer,owned); }
        }
    }

    public static void run(boolean budgets) throws Exception {
        ExecutorService callbacks=Executors.newFixedThreadPool(4);
        if (budgets) {
            try { completedUploads(callbacks); pausedReader(callbacks); goAwayBeforeHeaders(callbacks); }
            catch (Exception | Error failure) { callbacks.shutdownNow(); throw failure; }
        }
        try (Peer peer=new Peer()) {
            HttpEngine engine=new HttpEngine.Builder(new android.content.ContextWrapper(null)).build();
            List<Call> owned=new ArrayList<>();
            Throwable primary=null;
            try {
                int repetitions=64;
                long begin=System.nanoTime();
                for (int i=0;i<repetitions;i++) {
                    Call call=new Call(engine,peer.url(),callbacks); owned.add(call); call.start();
                    check(call.await().equals("success") && call.terminals.get()==1,"sequential terminal/body");
                }
                long elapsed=System.nanoTime()-begin;
                int connections=peer.accepted.get();
                System.out.println("HTTP2_SESSIONS requests="+repetitions+" connections="+connections
                        +" bodies="+(repetitions*BODY.length)+" elapsed_ns="+elapsed);
                if (budgets) check(connections==1,"sequential requests did not reuse one physical session");
                peer.hold=true;
                List<Call> active=new ArrayList<>();
                for (int i=0;i<16;i++) {
                    Call call=new Call(engine,peer.url(),callbacks); owned.add(call); active.add(call); call.start();
                }
                for (Call call:active) check(call.headers.await(5,TimeUnit.SECONDS),"concurrent headers missing");
                try { engine.shutdown(); throw new AssertionError("active engine shutdown accepted"); }
                catch (IllegalStateException expected) { checks++; }
                active.get(0).stream.cancel();
                check(active.get(0).await().equals("canceled"),"one stream cancellation");
                if (budgets) {
                    await(() -> peer.resets.get()==1,"cancellation did not send one RST_STREAM");
                    check(peer.accepted.get()==1 && peer.open.get()==1,"cancellation closed a shared session");
                } else {
                    await(() -> peer.open.get()==15,"baseline canceled physical session did not retire");
                }
                Call replacement=null;
                if (budgets) {
                    peer.resetOne();
                    await(() -> active.stream().filter(call -> call.terminal.isCompletedExceptionally()).count()==1,
                            "peer RST did not terminate exactly one stream");
                    int before=peer.accepted.get(); peer.goAway();
                    // GOAWAY retires admission while accepted streams continue.
                    // Inspect the protocol state transition, not a guessed sleep.
                    await(() -> sessionsRetired(engine),"GOAWAY did not retire session admission");
                    replacement=new Call(engine,peer.url(),callbacks); owned.add(replacement); replacement.start();
                    check(replacement.headers.await(5,TimeUnit.SECONDS),"replacement headers after active GOAWAY");
                    check(peer.accepted.get()==before+1 && peer.open.get()==2,
                            "GOAWAY closed existing streams or failed to replace admission");
                }
                peer.finishAll();
                int rejected=0;
                for (int i=1;i<active.size();i++) {
                    try { check(active.get(i).await().equals("success"),"peer stream canceled"); }
                    catch (ExecutionException expectedReset) {
                        if (!budgets || !(expectedReset.getCause() instanceof HttpException)) throw expectedReset;
                        rejected++;
                    }
                }
                check(rejected==(budgets?1:0),"peer reset affected the wrong number of streams");
                for (Call call:active) check(call.terminals.get()==1,"duplicate terminal");
                if (budgets) {
                    check(replacement.await().equals("success"),"replacement body after active GOAWAY");
                    check(replacement.terminals.get()==1,"duplicate replacement terminal");
                    await(() -> peer.open.get()==1,"drained GOAWAY session did not retire");
                } else {
                    await(() -> peer.open.get()==0,"baseline private sessions did not retire");
                    int before=peer.accepted.get();
                    Call after=new Call(engine,peer.url(),callbacks); owned.add(after); after.start();
                    check(after.await().equals("success"),"subsequent baseline request");
                    check(peer.accepted.get()==before+1,"private baseline connection count");
                }
                engine.shutdown();
                await(() -> peer.open.get()==0,"engine shutdown retained owned socket(s)");
                peer.healthy();
            } catch (Exception | Error failure) {
                primary=failure; throw failure;
            } finally {
                try {
                    for (Call call:owned) call.stream.cancel();
                    for (Call call:owned) {
                        try { call.await(); } catch (ExecutionException expectedFailure) { }
                    }
                    engine.shutdown();
                } catch (Exception | Error cleanup) {
                    if (primary!=null) primary.addSuppressed(cleanup); else throw cleanup;
                }
            }
        } finally { callbacks.shutdownNow(); }
        System.out.println("Http2DeviceSmoke passed: HTTP/2 session contracts "+checks+" checks; budgets="+budgets);
    }
}
