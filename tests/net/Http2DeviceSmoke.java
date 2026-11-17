// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.net.*;
import java.io.*;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;

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

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("disabled")) {
            checkHttp2Disabled();
            return;
        }
        boolean gzipHeaders=args.length>0 && args[0].equals("gzip-headers");
        boolean earlyRead=args.length>0 && args[0].equals("early-read");
        boolean readOnly=args.length>0 && args[0].equals("readonly-write");
        boolean gzip=gzipHeaders || (args.length>0 && args[0].equals("gzip"));
        boolean unary=gzip || earlyRead || readOnly || (args.length>0 && args[0].equals("unary"));
        boolean tls=args.length>0 && !unary;
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
                    ByteBuffer b=ByteBuffer.allocateDirect(4); b.put(new byte[]{'p','i','n','g'}).flip();
                    s.write(readOnly ? b.asReadOnlyBuffer() : b,unary);
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
                if (!earlyRead) s.read(ByteBuffer.allocateDirect(32));
            }
            public void onReadCompleted(BidirectionalStream s,UrlResponseInfo i,ByteBuffer b,boolean end) {
                b.flip(); while (b.hasRemaining()) body.append((char)b.get()); b.clear();
                if (!tls && !unary && !secondWrite[0] && body.length()>=4) {
                    secondWrite[0]=true; ByteBuffer w=ByteBuffer.allocateDirect(4); w.put(new byte[]{'p','o','n','g'}).flip(); s.write(w,true); s.flush();
                }
                if (!end && !unary) s.read(b);
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
            if (!tls && (!body.toString().equals(unary?"ping":"pingpong") || !trailers[0] || (!unary && !secondWrite[0]))) throw new AssertionError("Duplex/trailers failed: "+body);
            System.out.println(tls ? "Http2DeviceSmoke passed: TLS ALPN and HTTP/2 via configured proxy" : "Http2DeviceSmoke passed: full duplex, flush, direct buffers and gRPC trailers");
        } finally {
            stream.cancel(); engine.shutdown(); executor.shutdownNow(); if (server!=null) server.close();
        }
    }
}
