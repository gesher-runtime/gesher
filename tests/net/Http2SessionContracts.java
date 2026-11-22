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
            int last;
            Client(Socket socket) throws IOException { this.socket=socket; out=socket.getOutputStream(); }
            synchronized void send(int type,int flags,int stream,byte[] bytes) throws IOException {
                Http2DeviceSmoke.frame(out,type,flags,stream,bytes);
            }
            synchronized void finish(int id) throws IOException {
                if (!pending.remove(id)) return;
                send(0,1,id,BODY);
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
                        if (type==4 && (flags&1)==0) send(4,1,0,new byte[0]);
                        if (type==4 && (flags&1)!=0) settingsAcks.incrementAndGet();
                        if (type==6 && (flags&1)==0) send(6,1,0,bytes);
                        if (type==1) synchronized (this) {
                            if (id<=last || (id&1)==0 || (flags&4)==0) throw new IOException("stream identity/header order");
                            last=id; pending.add(id); requests.incrementAndGet();
                            send(1,4,id,new byte[]{(byte)0x88});
                            if (!hold) finish(id);
                        }
                        if (type==3) synchronized (this) {
                            if (bytes.length!=4) throw new IOException("bad RST_STREAM");
                            pending.remove(id); resets.incrementAndGet();
                        }
                    }
                } catch (SSLHandshakeException rejected) {
                    rejectedHandshakes.incrementAndGet();
                    if (!expectHandshakeFailure) failure.compareAndSet(null,rejected);
                } catch (EOFException | SocketException ended) {
                    // A baseline private session or an engine shutdown owns EOF.
                } catch (Throwable error) { if (!closing) failure.compareAndSet(null,error); }
                finally {
                    synchronized (this) { pending.clear(); }
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
        Call(HttpEngine engine, String url, Executor executor) {
            this(engine,url,executor,false);
        }
        Call(HttpEngine engine, String url, Executor executor, boolean delay) {
            stream=engine.newBidirectionalStreamBuilder(url,executor,this).setHttpMethod("GET")
                    .setDelayRequestHeadersUntilFirstFlushEnabled(delay).build();
        }
        void start() { stream.start(); }
        String await() throws Exception { return terminal.get(8,TimeUnit.SECONDS); }
        public void onStreamReady(BidirectionalStream stream) { ready.countDown(); }
        public void onResponseHeadersReceived(BidirectionalStream stream,UrlResponseInfo info) {
            if (info.getHttpStatusCode()!=200 || !info.getNegotiatedProtocol().equals("h2")) {
                throw new AssertionError("response metadata");
            }
            headers.countDown(); stream.read(buffer);
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

    public static void run(boolean budgets) throws Exception {
        ExecutorService callbacks=Executors.newFixedThreadPool(4);
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
                    java.lang.reflect.Field poolField=engine.getClass().getDeclaredField("http2Sessions");
                    poolField.setAccessible(true); Object pool=poolField.get(engine);
                    java.lang.reflect.Field sessions=pool.getClass().getDeclaredField("sessions"); sessions.setAccessible(true);
                    await(() -> {
                        try { synchronized (pool) {
                            for (Object session:(List<?>)sessions.get(pool)) {
                                java.lang.reflect.Method healthy=session.getClass().getDeclaredMethod("healthy");healthy.setAccessible(true);
                                if ((Boolean)healthy.invoke(session)) return false;
                            }
                            return true;
                        }} catch (Exception failure) { throw new RuntimeException(failure); }
                    },"GOAWAY did not retire session admission");
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
