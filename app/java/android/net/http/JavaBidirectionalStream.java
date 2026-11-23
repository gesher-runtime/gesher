// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import android.net.Network;
import java.net.ProtocolException;
import java.net.URL;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.Executor;

/** Full-duplex HTTP/2 streams, using AOSP's existing HPACK and flow control. */
final class JavaBidirectionalStream extends BidirectionalStream {
    final JavaHttpEngine engine;
    final URL url;
    final String method;
    final JavaHttpEngine.Headers headers;
    final Network network;
    final Callback callback;
    final JavaHttpEngine.SerialExecutor callbacks, writer;
    final int priority,tag,uid;
    final boolean hasTag,hasUid,delay;
    final ArrayDeque<Write> writes=new ArrayDeque<>();
    volatile Http2Transport transport;
    volatile boolean done;
    private boolean started, ready, headersSent, readPending, readEnded, writeEnded, endQueued;
    private ByteBuffer pendingRead;
    private volatile Response info;
    private int initialHeaderCount;
    private int pendingCallbacks;
    private int ioWorkers;
    private boolean cleanupDone, terminalReleased;
    private HttpException terminalError;
    private boolean terminalCanceled;
    private Response terminalInfo;
    private static final int TRANSFER_BYTES=32768;
    // Only the serial writer uses these bytes. Terminal publication drops its
    // reference without modifying a buffer still owned by an in-flight write.
    private byte[] writeScratch;
    private static void trace(String value) {
        if ("1".equals(System.getenv("GESHER_HTTP_ENGINE_TRACE"))) System.err.println("HttpEngine stream: "+value);
    }
    JavaBidirectionalStream(Builder b) {
        engine=b.engine; url=b.url; method=b.method; headers=new JavaHttpEngine.Headers(b.headers);
        network=b.network; callback=b.callback;
        callbacks=new JavaHttpEngine.SerialExecutor(b.executor); writer=new JavaHttpEngine.SerialExecutor(engine.io);
        priority=b.priority; tag=b.tag; uid=b.uid; hasTag=b.hasTag; hasUid=b.hasUid; delay=b.delay;
        writeEnded=method.equals("GET") || method.equals("HEAD"); endQueued=writeEnded;
    }
    @Override public HeaderBlock getHeaders() { return headers; }
    @Override public String getHttpMethod() { return method; }
    @Override public int getPriority() { return priority; }
    @Override public int getTrafficStatsTag() { return tag; }
    @Override public int getTrafficStatsUid() { return uid; }
    @Override public boolean hasTrafficStatsTag() { return hasTag; }
    @Override public boolean hasTrafficStatsUid() { return hasUid; }
    @Override public boolean isDelayRequestHeadersUntilFirstFlushEnabled() { return delay; }
    @Override public boolean isDone() { return done; }
    private interface Action { void run() throws Exception; }
    private void callback(Action action) {
        synchronized (this) { pendingCallbacks++; }
        dispatchCallback(action);
    }
    private void dispatchCallback(Action action) {
        try { callbacks.execute(() -> {
            try {
                if (!done) action.run();
            } catch (Exception e) { fail(new CallbackException("Exception in stream callback",e){}); }
            finally {
                synchronized (this) { pendingCallbacks--; }
                succeedIfComplete();
            }
        }); } catch (RuntimeException e) {
            synchronized (this) { pendingCallbacks--; }
            fail(e);
        }
    }
    private void io(Action action, boolean writing) {
        synchronized (this) { if (done) return; ioWorkers++; }
        try { (writing ? writer : engine.io).execute(() -> {
            try { if (!done) action.run(); }
            catch (Exception e) { fail(e); }
            finally { workerRetired(); }
        }); } catch (RuntimeException e) { workerRetired(); fail(e); }
    }
    private void workerRetired() {
        synchronized (this) { ioWorkers--; }
        publishTerminal();
    }
    @Override public synchronized void start() {
        if (started) throw new IllegalStateException("Stream has already started");
        engine.register(this); started=true;
        io(() -> {
            // The current duplex transport requires HTTP/2. Fail through the
            // ordinary asynchronous callback path before opening a connection.
            if (!engine.http2Enabled) throw new ProtocolException("HTTP/2 is disabled for this engine");
            Http2Transport opened=new Http2Transport(engine.http2Sessions, engine.brotliEnabled);
            synchronized (this) { if (done) { opened.close(); return; } transport=opened; }
            opened.connect(url,network);
            synchronized (this) { if (done) return; ready=true; }
            callback(() -> callback.onStreamReady(this));
            if (!delay) sendHeaders();
        },true);
    }
    private void sendHeaders() throws Exception {
        if (headersSent) return;
        transport.start(url,method,engine.contentHeaders(url,headers.getAsList()),engine.agent,!writeEnded); headersSent=true;
        io(() -> {
            List<Map.Entry<String,String>> received=transport.headers();
            initialHeaderCount=received.size();
            int code=0;
            List<Map.Entry<String,String>> values=new ArrayList<>();
            for (Map.Entry<String,String> h:received) {
                if (h.getKey().equals(":status")) code=Integer.parseInt(h.getValue());
                // Cronet's HTTP/2 header block retains :status. gRPC consumes
                // that metadata directly, separately from getHttpStatusCode().
                values.add(h);
            }
            if (code<100) throw new java.io.IOException("HTTP/2 response is missing :status");
            trace("HTTP status "+code);
            transport.setResponseEncoding(method.equals("HEAD") || code == 204 || code == 304
                    ? "" : ContentDecoding.encoding(values));
            Response response=new Response(url.toExternalForm(),code,new JavaHttpEngine.Headers(values));
            synchronized (this) { if (done) return; info=response; }
            callback(() -> callback.onResponseHeadersReceived(this,response));
            pump();
        },false);
    }
    @Override public synchronized void write(ByteBuffer buffer, boolean endOfStream) {
        Objects.requireNonNull(buffer);
        if (!buffer.isDirect() || (!buffer.hasRemaining() && !endOfStream))
            throw new IllegalArgumentException("A nonempty direct buffer is required");
        if (done) return;
        if (!ready || endQueued) throw new IllegalStateException("Stream cannot accept a write");
        writes.add(new Write(buffer,endOfStream));
        if (endOfStream) endQueued=true;
    }
    @Override public synchronized void flush() {
        if (done) return;
        if (!ready) throw new IllegalStateException("Stream is not ready");
        List<Write> batch=new ArrayList<>(writes); writes.clear();
        io(() -> {
            sendHeaders();
            for (Write write:batch) {
                for (;;) {
                    byte[] bytes;
                    int count;
                    synchronized (this) {
                        if (done) return;
                        if (!write.buffer.hasRemaining()) break;
                        int desired=Math.min(TRANSFER_BYTES,write.buffer.remaining());
                        if (writeScratch==null || writeScratch.length<desired) {
                            int capacity=writeScratch==null ? Math.max(256,desired)
                                    : Math.max(writeScratch.length*2,desired);
                            writeScratch=new byte[Math.min(TRANSFER_BYTES,capacity)];
                        }
                        bytes=writeScratch;
                        count=Math.min(bytes.length,write.buffer.remaining());
                        write.buffer.get(bytes,0,count);
                    }
                    transport.write(bytes,0,count);
                }
                if (done) return;
                transport.flush(write.end);
                // Reserve the callback before exposing END_STREAM to the reader.
                // Otherwise its EOF can complete the stream and drop this write.
                synchronized (this) { if (write.end) writeEnded=true; pendingCallbacks++; }
                dispatchCallback(() -> callback.onWriteCompleted(this,info,write.buffer,write.end));
            }
            callbacks.execute(this::succeedIfComplete);
        },true);
    }
    @Override public synchronized void read(ByteBuffer buffer) {
        Objects.requireNonNull(buffer);
        if (!buffer.isDirect() || buffer.isReadOnly() || !buffer.hasRemaining())
            throw new IllegalArgumentException("A nonempty writable direct buffer is required");
        if (done) return;
        if (!ready || readPending) throw new IllegalStateException("Stream cannot accept a read");
        if (readEnded) { callback(() -> callback.onReadCompleted(this,info,buffer,true)); return; }
        readPending=true; pendingRead=buffer; notifyAll();
    }
    /** Keep at most one bounded chunk ahead of application demand. In
     * particular, consume END_STREAM/trailers after the last message without
     * requiring an extra application read: gRPC stops reading at message end.
     */
    private void pump() throws Exception {
        byte[] bytes=new byte[TRANSFER_BYTES];
        for (;;) {
            int length;
            try { length=transport.read(bytes,0,bytes.length); }
            finally { info.received=transport.receivedBytes(); }
            if (done) return;
            if (length<0) {
                List<Map.Entry<String,String>> all=transport.headers();
                List<Map.Entry<String,String>> trailing=new ArrayList<>();
                for (int index=0;index<all.size();index++) {
                    Map.Entry<String,String> h=all.get(index);
                    // AOSP's framed stream merges successive HEADERS blocks. A
                    // fast trailers-only gRPC response can arrive before our
                    // first snapshot. Always deliver its terminal metadata.
                    if (index>=initialHeaderCount || h.getKey().equals("grpc-status")
                            || h.getKey().equals("grpc-message") || h.getKey().equals("grpc-status-details-bin")) trailing.add(h);
                }
                if (!trailing.isEmpty()) {
                    for (Map.Entry<String,String> h:trailing) if (h.getKey().equals("grpc-status") && h.getValue().matches("[0-9]+")) trace("gRPC status "+h.getValue());
                    HeaderBlock trailers=new JavaHttpEngine.Headers(trailing);
                    callback(() -> callback.onResponseTrailersReceived(this,info,trailers));
                }
                ByteBuffer buffer;
                synchronized (this) {
                    readEnded=true; buffer=pendingRead; pendingRead=null; readPending=false;
                    if (buffer!=null) pendingCallbacks++;
                }
                if (buffer!=null) dispatchCallback(() -> callback.onReadCompleted(this,info,buffer,true));
                callbacks.execute(this::succeedIfComplete);
                return;
            }
            int offset=0;
            while (offset<length) {
                ByteBuffer buffer;
                synchronized (this) {
                    while (pendingRead==null && !done) wait();
                    if (done) return;
                    buffer=pendingRead; pendingRead=null; readPending=false;
                    int count=Math.min(buffer.remaining(),length-offset);
                    buffer.put(bytes,offset,count); offset+=count;
                }
                callback(() -> callback.onReadCompleted(this,info,buffer,false));
            }
        }
    }
    private void succeedIfComplete() {
        synchronized (this) { if (!readEnded || !writeEnded || pendingCallbacks!=0 || done) return; }
        finish(null,false);
    }
    @Override public void cancel() {
        synchronized(this) { if (!started) return; }
        finish(null,true);
    }
    private void fail(Exception e) {
        if (done) return;
        // Names and code locations only: never log URLs, headers, request or response bodies.
        if ("1".equals(System.getenv("GESHER_HTTP_ENGINE_TRACE"))) {
            for (Throwable t=e;t!=null;t=t.getCause()) System.err.println("HttpEngine stream failure: "+t.getClass().getName()
                    +(t.getStackTrace().length==0 ? "" : " at "+t.getStackTrace()[0]));
        }
        finish(e instanceof HttpException ? (HttpException)e : new NetworkException("HTTP/2 stream failed",e) {
        @Override public int getErrorCode() { return ERROR_OTHER; }
        @Override public boolean isImmediatelyRetryable() { return false; }
    },false); }
    private void finish(HttpException error, boolean canceled) {
        synchronized (this) {
            if (done) return;
            done=true; terminalInfo=info; terminalError=error; terminalCanceled=canceled;
            writes.clear(); pendingRead=null; writeScratch=null; notifyAll();
            ioWorkers++;
        }
        trace(canceled ? "canceled" : error==null ? "succeeded" : "failed");
        Runnable cleanup=() -> {
            try { Http2Transport t=transport; if (t!=null) t.close(); }
            catch (Exception ignored) { }
            finally {
                synchronized (this) { cleanupDone=true; }
                workerRetired();
            }
        };
        try { engine.io.execute(cleanup); }
        catch (RuntimeException rejected) { cleanup.run(); }
    }
    private void publishTerminal() {
        synchronized (this) {
            if (!done || !cleanupDone || ioWorkers!=0 || terminalReleased) return;
            terminalReleased=true;
        }
        engine.release(this);
        try { callbacks.execute(() -> {
            try {
                if (terminalCanceled) callback.onCanceled(this,terminalInfo);
                else if (terminalError!=null) callback.onFailed(this,terminalInfo,terminalError);
                else callback.onSucceeded(this,terminalInfo);
            } catch (RuntimeException ignored) { }
        }); } catch (RuntimeException ignored) { }
    }
    private static final class Write {
        final ByteBuffer buffer; final boolean end;
        Write(ByteBuffer buffer,boolean end) { this.buffer=buffer; this.end=end; }
    }
    private static final class Response extends UrlResponseInfo {
        final String url;
        final int code;
        final HeaderBlock headers;
        volatile long received;
        Response(String url,int code,HeaderBlock headers) { this.url=url; this.code=code; this.headers=headers; }
        @Override public HeaderBlock getHeaders() { return headers; }
        @Override public int getHttpStatusCode() { return code; }
        @Override public String getHttpStatusText() { return ""; }
        @Override public String getNegotiatedProtocol() { return "h2"; }
        @Override public long getReceivedByteCount() { return received; }
        @Override public String getUrl() { return url; }
        @Override public List<String> getUrlChain() { return Collections.singletonList(url); }
        @Override public boolean wasCached() { return false; }
    }
    static final class Builder extends BidirectionalStream.Builder {
        final JavaHttpEngine engine;
        final URL url;
        final Executor executor;
        final Callback callback;
        final Network network;
        final List<Map.Entry<String,String>> headers=new ArrayList<>();
        String method="POST";
        int priority=STREAM_PRIORITY_MEDIUM,tag,uid;
        boolean hasTag,hasUid,delay;
        Builder(JavaHttpEngine e,String url,Executor executor,Callback callback,Network network) {
            engine=e; this.url=JavaHttpEngine.url(url); this.executor=Objects.requireNonNull(executor);
            this.callback=Objects.requireNonNull(callback); this.network=network;
        }
        @Override public BidirectionalStream.Builder addHeader(String n,String v) { JavaHttpEngine.header(headers,n,v); return this; }
        @Override public BidirectionalStream.Builder setHttpMethod(String v) { method=JavaHttpEngine.token(v); return this; }
        @Override public BidirectionalStream.Builder setDelayRequestHeadersUntilFirstFlushEnabled(boolean v) { delay=v; return this; }
        @Override public BidirectionalStream.Builder setPriority(int v) { if (v<0 || v>4) throw new IllegalArgumentException("Invalid stream priority"); priority=v; return this; }
        @Override public BidirectionalStream.Builder setTrafficStatsTag(int v) { tag=v; hasTag=true; return this; }
        @Override public BidirectionalStream.Builder setTrafficStatsUid(int v) { uid=v; hasUid=true; return this; }
        @Override public BidirectionalStream build() { engine.checkOpen(); return new JavaBidirectionalStream(this); }
    }
}
