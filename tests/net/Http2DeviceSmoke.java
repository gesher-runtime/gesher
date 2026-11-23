// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.net.*;
import java.io.*;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Actual ART/OkHttp framing, against a small independent HTTP/2 wire peer.
 * The peer does not decode requests: it returns static HPACK response headers,
 * echoes each DATA frame before END_STREAM and finishes with grpc-status trailers.
 */
public class Http2DeviceSmoke {
    static void frame(OutputStream out,int type,int flags,int stream,byte[] data) throws IOException {
        int n=data.length;
        out.write(new byte[]{(byte)(n>>16),(byte)(n>>8),(byte)n,(byte)type,(byte)flags,
                (byte)(stream>>24),(byte)(stream>>16),(byte)(stream>>8),(byte)stream});
        out.write(data); out.flush();
    }
    static byte[] literal(String name,String value) throws IOException {
        ByteArrayOutputStream b=new ByteArrayOutputStream(); b.write(0); b.write(name.length());
        b.write(name.getBytes("US-ASCII")); b.write(value.length()); b.write(value.getBytes("US-ASCII")); return b.toByteArray();
    }
    static void checkHttp2Disabled() throws Exception {
        HttpEngine.Builder builder = new HttpEngine.Builder(new android.content.ContextWrapper(null));
        HttpEngine disabled = builder.setEnableHttp2(false).build();
        builder.setEnableHttp2(true);  // The already built engine must keep its own value.
        ExecutorService executor = Executors.newSingleThreadExecutor();
        try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))) {
            CompletableFuture<Void> failed = new CompletableFuture<>();
            String url = "http://127.0.0.1:" + server.getLocalPort() + "/grpc";
            BidirectionalStream stream = disabled.newBidirectionalStreamBuilder(url, executor,
                    new BidirectionalStream.Callback() {
                        @Override public void onStreamReady(BidirectionalStream s) {
                            failed.completeExceptionally(new AssertionError("Disabled HTTP/2 stream became ready"));
                        }

                        @Override public void onFailed(BidirectionalStream s, UrlResponseInfo i,
                                HttpException e) {
                            if (!(e.getCause() instanceof ProtocolException)) failed.completeExceptionally(e);
                            else if (i != null) failed.completeExceptionally(
                                    new AssertionError("Failure before headers fabricated response information"));
                            else failed.complete(null);
                        }
                        @Override public void onResponseHeadersReceived(BidirectionalStream s, UrlResponseInfo i) {}
                        @Override public void onReadCompleted(BidirectionalStream s, UrlResponseInfo i,
                                ByteBuffer b, boolean end) {}
                        @Override public void onWriteCompleted(BidirectionalStream s, UrlResponseInfo i,
                                ByteBuffer b, boolean end) {}
                        @Override public void onResponseTrailersReceived(BidirectionalStream s,
                                UrlResponseInfo i, HeaderBlock trailers) {}

                        @Override public void onSucceeded(BidirectionalStream s, UrlResponseInfo i) {
                            failed.completeExceptionally(new AssertionError("Disabled HTTP/2 stream succeeded"));
                        }

                        @Override public void onCanceled(BidirectionalStream s, UrlResponseInfo i) {
                            failed.completeExceptionally(new AssertionError("Disabled HTTP/2 stream was canceled"));
                        }
                    }).build();
            try {
                stream.start();
                failed.get(5, TimeUnit.SECONDS);
                if (!stream.isDone()) throw new AssertionError("Disabled stream is still active");
                server.setSoTimeout(200);
                try (Socket unexpected = server.accept()) {
                    throw new AssertionError("Disabled HTTP/2 opened a connection");
                } catch (SocketTimeoutException expected) {
                    // The independent socket peer must never see a connection attempt.
                }
            } finally {
                stream.cancel();
            }
        } finally {
            disabled.shutdown();
            executor.shutdownNow();
        }
        System.out.println("Http2DeviceSmoke passed: HTTP/2 opt-out fires failure without connecting");
    }

    static boolean stackHas(StackTraceElement[] stack,String owner,String method) {
        for (StackTraceElement frame:stack) {
            if (frame.getClassName().equals(owner) && frame.getMethodName().equals(method)) return true;
        }
        return false;
    }
    static boolean streamIoStack(StackTraceElement[] stack) {
        for (StackTraceElement frame:stack) {
            String owner=frame.getClassName();
            if (owner.equals("android.net.http.Http2Transport")
                    || owner.startsWith("android.net.http.JavaBidirectionalStream")) return true;
        }
        return false;
    }
    static void checkCancellationFailure(AtomicReference<Throwable> failure) {
        if (failure.get()!=null) throw new AssertionError("Unexpected cancellation callback or peer failure",failure.get());
    }
    static void cancellationFailure(AtomicReference<Throwable> failure,CompletableFuture<Void> canceled,Throwable error) {
        // Completing an already canceled future cannot expose a later failure.
        failure.compareAndSet(null,error); canceled.completeExceptionally(error);
    }
    static Thread[] awaitBlockedDuplex(AtomicReference<Throwable> failure) throws Exception {
        long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
        String diagnostic="";
        do {
            checkCancellationFailure(failure);
            Thread reader=null,writer=null;
            StringBuilder observed=new StringBuilder();
            for (Map.Entry<Thread,StackTraceElement[]> entry:Thread.getAllStackTraces().entrySet()) {
                Thread thread=entry.getKey(); StackTraceElement[] stack=entry.getValue();
                if (!streamIoStack(stack)) continue;
                Thread.State state=thread.getState();
                observed.append(thread.getName()).append(':').append(state).append(Arrays.toString(stack)).append('\n');
                if (state!=Thread.State.WAITING && state!=Thread.State.TIMED_WAITING) continue;
                String framed="com.android.okhttp.internal.framed.";
                if (stackHas(stack,framed+"FramedStream$FramedDataSource","waitUntilReadable")
                        && stackHas(stack,framed+"FramedStream$FramedDataSource","read")) reader=thread;
                if (stackHas(stack,framed+"FramedStream$FramedDataSink","emitDataFrame")
                        || stackHas(stack,framed+"FramedConnection","writeData")) writer=thread;
            }
            if (reader!=null && writer!=null && reader!=writer) return new Thread[]{reader,writer};
            diagnostic=observed.toString();
            Thread.sleep(10);
        } while (System.nanoTime()<deadline);
        throw new AssertionError("Both HTTP/2 directions did not enter their AOSP waits:\n"+diagnostic);
    }
    static void awaitStreamIoRetirement(Thread[] workers,AtomicReference<Throwable> failure) throws Exception {
        long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
        String diagnostic="";
        do {
            checkCancellationFailure(failure);
            StringBuilder active=new StringBuilder();
            for (Thread worker:workers) {
                StackTraceElement[] stack=worker.getStackTrace();
                if (streamIoStack(stack)) active.append(worker.getName()).append(Arrays.toString(stack)).append('\n');
            }
            if (active.length()==0) return;
            diagnostic=active.toString();
            Thread.sleep(10);
        } while (System.nanoTime()<deadline);
        throw new AssertionError("Canceled HTTP/2 worker still owns production I/O:\n"+diagnostic);
    }

    /** Cancel while the reader has no response DATA and the writer has no
     * WINDOW_UPDATE credit. Closing the transport must retire both directions. */
    static void checkCancellation() throws Exception {
        ExecutorService executor=Executors.newSingleThreadExecutor();
        HttpEngine engine=new HttpEngine.Builder(new android.content.ContextWrapper(null)).build();
        CountDownLatch headersReceived=new CountDownLatch(1),windowConsumed=new CountDownLatch(1),peerClosed=new CountDownLatch(1);
        CompletableFuture<Void> canceled=new CompletableFuture<>();
        AtomicReference<Throwable> unexpected=new AtomicReference<>();
        AtomicInteger terminals=new AtomicInteger(),dataBytes=new AtomicInteger();
        AtomicBoolean cancelRequested=new AtomicBoolean();
        try (ServerSocket server=new ServerSocket(0,1,InetAddress.getByName("127.0.0.1"))) {
            Thread peer=new Thread(() -> {
                try (Socket socket=server.accept()) {
                    socket.setSoTimeout(10000);
                    DataInputStream in=new DataInputStream(socket.getInputStream());
                    OutputStream out=socket.getOutputStream();
                    byte[] preface=new byte[24]; in.readFully(preface);
                    if (!new String(preface,java.nio.charset.StandardCharsets.US_ASCII)
                            .equals("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")) throw new IOException("Bad preface");
                    frame(out,4,0,0,new byte[0]);
                    for (;;) {
                        int n=(in.readUnsignedByte()<<16)|(in.readUnsignedByte()<<8)|in.readUnsignedByte();
                        int type=in.readUnsignedByte(),flags=in.readUnsignedByte(),id=in.readInt()&0x7fffffff;
                        if (type==0) {
                            if (dataBytes.get()+n>65535) throw new AssertionError("DATA exceeded the initial write window");
                            if (dataBytes.get()+n==65535) {
                                // The writer may retain the DATA frame's last bytes in
                                // BufferedSink while waiting for more credit. A PING
                                // ACK flushes them without sending WINDOW_UPDATE.
                                frame(out,6,0,0,new byte[8]);
                            }
                        }
                        byte[] data=new byte[n]; in.readFully(data);
                        if (type==4 && (flags&1)==0) frame(out,4,1,0,new byte[0]);
                        if (type==1) frame(out,1,4,id,new byte[]{(byte)0x88});
                        if (type==0) {
                            if ((flags&1)!=0) throw new AssertionError("Upload ended without enough flow-control credit");
                            if (dataBytes.addAndGet(n)==65535) windowConsumed.countDown();
                        }
                        // Deliberately give no DATA response and no write credit.
                    }
                } catch (EOFException expected) {
                    // Peer EOF confirms that cancellation closed the real socket.
                    if (!cancelRequested.get()) cancellationFailure(unexpected,canceled,expected);
                } catch (SocketException reset) {
                    // An abortive TCP close is also a terminal peer-close result.
                    // Timeout and errors before our cancellation still fail.
                    if (!cancelRequested.get()) cancellationFailure(unexpected,canceled,reset);
                } catch (Throwable failure) { cancellationFailure(unexpected,canceled,failure); }
                finally { peerClosed.countDown(); }
            },"http2-cancel-peer");
            peer.setDaemon(true); peer.start();
            BidirectionalStream stream=engine.newBidirectionalStreamBuilder(
                    "http://127.0.0.1:"+server.getLocalPort()+"/blocked",executor,
                    new BidirectionalStream.Callback() {
                        public void onStreamReady(BidirectionalStream s) {
                            ByteBuffer body=ByteBuffer.allocateDirect(512*1024);
                            s.write(body,true); s.flush();
                        }
                        public void onResponseHeadersReceived(BidirectionalStream s,UrlResponseInfo i) {
                            headersReceived.countDown();
                        }
                        public void onReadCompleted(BidirectionalStream s,UrlResponseInfo i,ByteBuffer b,boolean end) {
                            cancellationFailure(unexpected,canceled,new AssertionError("peer sent no response body"));
                        }
                        public void onWriteCompleted(BidirectionalStream s,UrlResponseInfo i,ByteBuffer b,boolean end) {
                            cancellationFailure(unexpected,canceled,new AssertionError("write finished without flow-control credit"));
                        }
                        public void onResponseTrailersReceived(BidirectionalStream s,UrlResponseInfo i,HeaderBlock h) {
                            cancellationFailure(unexpected,canceled,new AssertionError("peer sent no trailers"));
                        }
                        public void onSucceeded(BidirectionalStream s,UrlResponseInfo i) {
                            terminals.incrementAndGet(); cancellationFailure(unexpected,canceled,new AssertionError("canceled stream succeeded"));
                        }
                        public void onFailed(BidirectionalStream s,UrlResponseInfo i,HttpException failure) {
                            terminals.incrementAndGet(); cancellationFailure(unexpected,canceled,failure);
                        }
                        public void onCanceled(BidirectionalStream s,UrlResponseInfo i) {
                            terminals.incrementAndGet();
                            if (!cancelRequested.get()) cancellationFailure(unexpected,canceled,
                                    new AssertionError("stream canceled before the test requested cancellation"));
                            else canceled.complete(null);
                        }
                    }).setDelayRequestHeadersUntilFirstFlushEnabled(true).build();
            try {
                stream.start();
                if (!headersReceived.await(5,TimeUnit.SECONDS)) {
                    checkCancellationFailure(unexpected); throw new AssertionError("response headers did not arrive");
                }
                if (!windowConsumed.await(5,TimeUnit.SECONDS)) {
                    checkCancellationFailure(unexpected);
                    throw new AssertionError("writer did not exhaust initial credit; DATA bytes="+dataBytes.get());
                }
                Thread[] workers=awaitBlockedDuplex(unexpected);
                cancelRequested.set(true); stream.cancel(); canceled.get(10,TimeUnit.SECONDS);
                if (!peerClosed.await(5,TimeUnit.SECONDS)) throw new AssertionError("canceled socket remained open");
                awaitStreamIoRetirement(workers,unexpected);
                // Drain the production serial callback queue, not only its target
                // executor: queued callbacks can otherwise be submitted after a
                // barrier placed directly in the target executor's queue.
                java.lang.reflect.Field callbacks=stream.getClass().getDeclaredField("callbacks");
                callbacks.setAccessible(true);
                CompletableFuture<Void> drained=new CompletableFuture<>();
                ((Executor)callbacks.get(stream)).execute(() -> drained.complete(null));
                drained.get(5,TimeUnit.SECONDS);
                checkCancellationFailure(unexpected);
                if (dataBytes.get()!=65535) throw new AssertionError("cancellation changed exhausted credit");
                if (!stream.isDone() || terminals.get()!=1) throw new AssertionError("terminal cancellation ownership");
                try {
                    java.lang.reflect.Field scratch=stream.getClass().getDeclaredField("writeScratch");
                    scratch.setAccessible(true);
                    if (scratch.get(stream)!=null) throw new AssertionError("terminal stream retains write scratch");
                } catch (NoSuchFieldException baseline) { /* Older stream allocated only temporary chunks. */ }
                System.out.println("HTTP2_CANCEL DATA="+dataBytes.get()+" reader="+workers[0].getName()+"#"+workers[0].getId()
                        +" writer="+workers[1].getName()+"#"+workers[1].getId()+" retired=true terminals="+terminals.get());
            } finally { stream.cancel(); }
        } finally { engine.shutdown(); executor.shutdownNow(); }
        System.out.println("Http2DeviceSmoke passed: blocked duplex cancellation and scratch retirement");
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("disabled")) {
            checkHttp2Disabled();
            return;
        }
        if (args.length==1 && args[0].equals("cancel")) {
            checkCancellation();
            return;
        }
        boolean gzipHeaders=args.length>0 && args[0].equals("gzip-headers");
        boolean earlyRead=args.length>0 && args[0].equals("early-read");
        boolean readOnly=args.length>0 && args[0].equals("readonly-write");
        boolean large=args.length>0 && args[0].equals("large");
        boolean gzip=gzipHeaders || (args.length>0 && args[0].equals("gzip"));
        boolean unary=gzip || earlyRead || readOnly || large || (args.length>0 && args[0].equals("unary"));
        boolean tls=args.length>0 && !unary;
        byte[] upload=new byte[large ? 1024*1024+37 : 4];
        if (large) for (int i=0;i<upload.length;i++) upload[i]=(byte)('a'+i%26);
        else System.arraycopy(new byte[]{'p','i','n','g'},0,upload,0,4);
        ServerSocket server=tls ? null : new ServerSocket(0,1,InetAddress.getByName("127.0.0.1"));
        Throwable[] failure=new Throwable[1];
        if (!tls) {
            Thread peer=new Thread(() -> {
                try (Socket s=server.accept()) {
                    s.setSoTimeout(10000); DataInputStream in=new DataInputStream(s.getInputStream()); OutputStream out=s.getOutputStream();
                    byte[] preface=new byte[24]; in.readFully(preface);
                    if (!new String(preface,"US-ASCII").equals("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")) throw new IOException("Bad preface");
                    frame(out,4,0,0,new byte[0]);
                    for (;;) {
                        int n=(in.readUnsignedByte()<<16)|(in.readUnsignedByte()<<8)|in.readUnsignedByte();
                        int type=in.readUnsignedByte(),flags=in.readUnsignedByte(),stream=in.readInt()&0x7fffffff;
                        byte[] data=new byte[n]; in.readFully(data);
                        if (type==4 && (flags&1)==0) frame(out,4,1,0,new byte[0]);
                        if (type==1) {
                            ByteArrayOutputStream headers=new ByteArrayOutputStream(); headers.write(0x88); // indexed :status 200
                            if (gzip) headers.write(literal("content-encoding","gzip"));
                            frame(out,1,4,stream,headers.toByteArray());
                        }
                        if (type==0) {
                            if (large && n>0) {
                                // Restore both request windows as DATA is consumed,
                                // so the >1 MiB write crosses real flow-control waits.
                                byte[] credit={(byte)(n>>>24),(byte)(n>>>16),(byte)(n>>>8),(byte)n};
                                frame(out,8,0,0,credit); frame(out,8,0,stream,credit);
                            }
                            if (data.length>0) {
                                if (gzip) {
                                    ByteArrayOutputStream compressed=new ByteArrayOutputStream();
                                    try (java.util.zip.GZIPOutputStream zipped=new java.util.zip.GZIPOutputStream(compressed)) { zipped.write(data); }
                                    data=compressed.toByteArray();
                                }
                                frame(out,0,0,stream,data);
                            }
                            if ((flags&1)!=0) { frame(out,1,5,stream,literal("grpc-status","0")); break; }
                        }
                    }
                    // Wait for client GOAWAY/close so the response isn't raced by TCP reset.
                    while(in.read()!=-1) {}
                } catch(Exception e) { failure[0]=e; }
            }); peer.setDaemon(true); peer.start();
        }
        HttpEngine engine=new HttpEngine.Builder(new android.content.ContextWrapper(null)).build();
        ExecutorService executor=Executors.newSingleThreadExecutor();
        CountDownLatch done=new CountDownLatch(1);
        StringBuilder body=new StringBuilder();
        boolean[] trailers={false}, secondWrite={false};
        int[] completedWrites={0};
        String url=tls ? args[0] : "http://127.0.0.1:"+server.getLocalPort()+"/grpc";
        if (tls && args.length>1) java.net.ProxySelector.setDefault(new java.net.ProxySelector() {
            public List<java.net.Proxy> select(java.net.URI uri) { return Collections.singletonList(new java.net.Proxy(java.net.Proxy.Type.HTTP,InetSocketAddress.createUnresolved("127.0.0.1",Integer.parseInt(args[1])))); }
            public void connectFailed(java.net.URI u,java.net.SocketAddress a,IOException e) {}
        });
        BidirectionalStream stream=engine.newBidirectionalStreamBuilder(url,executor,new BidirectionalStream.Callback() {
            public void onStreamReady(BidirectionalStream s) {
                if (earlyRead) s.read(ByteBuffer.allocateDirect(32));
                if (!tls && !gzipHeaders) {
                    ByteBuffer b=ByteBuffer.allocateDirect(upload.length); b.put(upload).flip();
                    s.write(readOnly || large ? b.asReadOnlyBuffer() : b,unary);
                }
                s.flush();
            }
            public void onResponseHeadersReceived(BidirectionalStream s,UrlResponseInfo i) {
                if (!tls && (i.getHttpStatusCode()!=200 || !Collections.singletonList("200").equals(i.getHeaders().getAsMap().get(":status"))))
                    throw new IllegalStateException("HTTP/2 :status metadata is missing");
                // The peer sends response headers before it receives any DATA.
                // Decoding gzip must not delay this callback until body bytes.
                if (gzipHeaders) {
                    ByteBuffer b=ByteBuffer.allocateDirect(4); b.put(new byte[]{'p','i','n','g'}).flip();
                    s.write(b,true); s.flush();
                }
                if (!earlyRead) s.read(ByteBuffer.allocateDirect(large ? 4093 : 32));
            }
            public void onReadCompleted(BidirectionalStream s,UrlResponseInfo i,ByteBuffer b,boolean end) {
                b.flip(); while (b.hasRemaining()) body.append((char)b.get()); b.clear();
                if (!tls && !unary && !secondWrite[0] && body.length()>=4) {
                    secondWrite[0]=true; ByteBuffer w=ByteBuffer.allocateDirect(4); w.put(new byte[]{'p','o','n','g'}).flip(); s.write(w,true); s.flush();
                }
                if (!end && (!unary || large)) s.read(b);
            }
            public void onResponseTrailersReceived(BidirectionalStream s,UrlResponseInfo i,HeaderBlock h) { trailers[0]=h.getAsMap().get("grpc-status").equals(Collections.singletonList("0")); }
            public void onWriteCompleted(BidirectionalStream s,UrlResponseInfo i,ByteBuffer b,boolean end) { completedWrites[0]++; }
            public void onSucceeded(BidirectionalStream s,UrlResponseInfo i) {
                if (!tls && completedWrites[0]!=(unary?1:2)) failure[0]=new AssertionError("Completed before write callbacks");
                done.countDown();
            }
            public void onFailed(BidirectionalStream s,UrlResponseInfo i,HttpException e) { failure[0]=e; done.countDown(); }
            public void onCanceled(BidirectionalStream s,UrlResponseInfo i) { failure[0]=new IOException("Canceled"); done.countDown(); }
        }).setHttpMethod(tls?"GET":"POST").setDelayRequestHeadersUntilFirstFlushEnabled(true).build();
        try {
            stream.start();
            if (!done.await(20,TimeUnit.SECONDS)) throw new AssertionError("HTTP/2 timeout");
            if (failure[0]!=null) throw new AssertionError("HTTP/2 failed",failure[0]);
            String expected=large ? new String(upload,"US-ASCII") : unary ? "ping" : "pingpong";
            if (!tls && (!body.toString().equals(expected) || !trailers[0] || (!unary && !secondWrite[0]))) throw new AssertionError("Duplex/trailers failed; bytes="+body.length());
            System.out.println(tls ? "Http2DeviceSmoke passed: TLS ALPN and HTTP/2 via configured proxy" : "Http2DeviceSmoke passed: full duplex, flush, direct buffers and gRPC trailers");
        } finally {
            stream.cancel(); engine.shutdown(); executor.shutdownNow(); if (server!=null) server.close();
        }
    }
}
