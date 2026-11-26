// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import android.net.Network;
import java.io.*;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.*;
import java.util.*;

/** A narrow adapter to AOSP OkHttp's HTTP/2 framing already in core-libart.jar.
 * Like Network's HttpURLConnectionFactory adapter, reflection keeps this out of
 * the framework compile-only class path; no app-supplied Cronet code is loaded.
 */
final class Http2Transport implements Closeable {
    private final Http2SessionPool pool;
    private final boolean privatePool;
    private final boolean brotliEnabled;
    private Http2SessionPool.Session lease;
    private Http2SessionPool.Route route;
    private int operations;
    private Object connection, stream, source, sink;
    private volatile boolean closed;
    private volatile InputStream decodedBody;
    private String responseEncoding="";
    private volatile long receivedBytes;
    private final Http2Bindings api=Http2Bindings.get();
    // Each direction retains its own mutable buffer. Only fixed provider
    // metadata is shared across transports; no request data enters the cache.
    private final Object readBuffer=api.buffer.newInstance();
    private final Object writeBuffer=api.buffer.newInstance();

    Http2Transport() throws ReflectiveOperationException { this(new Http2SessionPool(), true, false); }
    Http2Transport(Http2SessionPool pool) throws ReflectiveOperationException { this(pool, false, false); }
    Http2Transport(Http2SessionPool pool, boolean brotliEnabled) throws ReflectiveOperationException {
        this(pool, false, brotliEnabled);
    }
    private Http2Transport(Http2SessionPool pool, boolean privatePool, boolean brotliEnabled) throws ReflectiveOperationException {
        this.pool=pool; this.privatePool=privatePool; this.brotliEnabled=brotliEnabled;
    }
    private synchronized void enter() throws IOException {
        if (closed) throw new IOException("Stream closed");
        operations++;
    }
    private void leave() {
        synchronized (this) { operations--; }
        retireIfClosed();
    }
    private void retireIfClosed() {
        Http2SessionPool.Session owned;
        InputStream decoded;
        Object inbound;
        synchronized (this) {
            if (!closed || operations!=0) return;
            owned=lease; lease=null;
            decoded=decodedBody; decodedBody=null;
            inbound=source;
            connection=null; stream=null; source=null; sink=null;
        }
        try { if (decoded!=null) decoded.close(); } catch (IOException ignored) { }
        try { if (inbound!=null) invoke(api.sourceClose,inbound); } catch (Exception ignored) { }
        if (owned!=null) pool.release(owned);
        if (privatePool) pool.close();
    }
    private void reset(Object value) throws IOException {
        if (value==null) return;
        try {
            // A stream whose both directions completed is already closed
            // (RFC 9113 section 5.1). A request body ends with Sink.close, which
            // the provider records as closed rather than finished; resetting
            // that stream would add a spurious RST_STREAM to every success.
            if (!(Boolean)invoke(api.streamIsOpen,value)) return;
            invoke(api.streamCloseLater,value,api.cancel);
        } catch (Exception failure) { throw new IOException("Cannot retire HTTP/2 stream",failure); }
    }
    static Object invoke(Method method, Object target, Object... args) throws Exception {
        try { return method.invoke(target,args); }
        catch (InvocationTargetException e) {
            Throwable cause=e.getCause();
            if (cause instanceof Exception) throw (Exception)cause;
            throw new IOException("HTTP/2 transport failed",cause);
        }
    }
    static String authority(URL url, boolean includeDefaultPort) {
        String host=url.getHost();
        if (host.contains(":") && !host.startsWith("[")) host="["+host+"]";
        int port=url.getPort();
        if (port<0 && includeDefaultPort) port=url.getDefaultPort();
        return port<0 ? host : host+":"+port;
    }
    void connect(URL url, Network network) throws Exception {
        enter();
        try {
            Http2SessionPool.Route selected=new Http2SessionPool.Route(url,network);
            synchronized (this) { route=selected; }
            lease(selected);
        } finally { leave(); }
    }
    private void lease(Http2SessionPool.Route selected) throws Exception {
        Http2SessionPool.Session acquired=pool.acquire(selected);
        synchronized (this) { lease=acquired; }
        if (closed) { acquired.cancelConnectingOrWake(); throw new IOException("Stream closed"); }
        acquired.connect();
        synchronized (this) {
            if (closed) throw new IOException("Stream closed");
            connection=acquired.connection;
        }
    }
    /** The leased session stopped admitting streams before this request had one. */
    private void replaceSession() throws Exception {
        Http2SessionPool.Session retired;
        Http2SessionPool.Route selected;
        synchronized (this) {
            if (closed) throw new IOException("Stream closed");
            retired=lease; lease=null; connection=null; selected=route;
        }
        if (retired!=null) pool.release(retired);
        lease(selected);
    }
    void start(URL url, String method, List<Map.Entry<String,String>> headers, String agent, boolean hasBody) throws Exception {
        List<Object> encoded=new ArrayList<>();
        String host=authority(url,false);
        encoded.add(api.header.newInstance(":method",method));
        encoded.add(api.header.newInstance(":scheme",url.getProtocol()));
        encoded.add(api.header.newInstance(":authority",host));
        String path=url.getFile();
        if (path.isEmpty() || path.charAt(0)=='?') path="/"+path;
        encoded.add(api.header.newInstance(":path",path));
        boolean hasAgent=false;
        for (Map.Entry<String,String> h:headers) {
            String name=h.getKey().toLowerCase(Locale.ROOT);
            if (name.equals("connection") || name.equals("host") || name.equals("keep-alive")
                    || name.equals("transfer-encoding") || name.equals("upgrade") || name.equals("proxy-connection")) continue;
            if (name.equals("user-agent")) hasAgent=true;
            encoded.add(api.header.newInstance(name,h.getValue()));
        }
        if (!hasAgent) encoded.add(api.header.newInstance("user-agent",agent));
        enter();
        try {
            Object opened;
            try { opened=lease.start(encoded,hasBody,() -> closed); }
            catch (Http2SessionPool.SessionUnavailableException retired) {
                // GOAWAY or connection loss raced session selection. No stream
                // id was allocated and no request byte was written, so the
                // request is still unsent: admit it once on a fresh session.
                replaceSession();
                opened=lease.start(encoded,hasBody,() -> closed);
            }
            Object inbound=invoke(api.streamSource,opened);
            synchronized (this) {
                stream=opened; source=inbound;
                if (closed) { reset(opened); throw new IOException("Stream closed"); }
            }
            invoke(api.timeout,invoke(api.streamReadTimeout,opened),30L,java.util.concurrent.TimeUnit.SECONDS);
            invoke(api.timeout,invoke(api.streamWriteTimeout,opened),30L,java.util.concurrent.TimeUnit.SECONDS);
            if (hasBody) sink=invoke(api.streamSink,opened);
            invoke(api.connectionFlush,connection);
        } finally { leave(); }
    }
    List<Map.Entry<String,String>> headers() throws Exception {
        enter();
        try {
        List<?> headers=(List<?>)invoke(api.streamResponseHeaders,stream);
        List<Map.Entry<String,String>> result=new ArrayList<>();
        for (Object h:headers) {
            Object name=api.headerName.get(h), value=api.headerValue.get(h);
            result.add(new AbstractMap.SimpleImmutableEntry<>((String)invoke(api.bytesUtf8,name),
                    (String)invoke(api.bytesUtf8,value)));
        }
        return result;
        } finally { leave(); }
    }
    void setResponseEncoding(String encoding) throws IOException {
        responseEncoding=encoding;
    }
    long receivedBytes() { return receivedBytes; }
    private void initDecodedBody() throws IOException {
        InputStream raw=new InputStream() {
            private final byte[] one=new byte[1];
            @Override public int read() throws IOException {
                int n=read(one,0,1); return n<0 ? -1 : one[0]&255;
            }
            @Override public int read(byte[] bytes,int offset,int count) throws IOException {
                Objects.checkFromIndexSize(offset,count,bytes.length);
                if (count==0) return 0;
                try {
                    return readRaw(bytes,offset,count);
                } catch (IOException e) { throw e; }
                catch (Exception e) { throw new IOException("HTTP/2 body read failed",e); }
            }
        };
        // Decoder initialization and all input reads happen only in the body
        // pump, after the application's response-header callback was dispatched.
        InputStream decoded=ContentDecoding.wrap(raw,responseEncoding,brotliEnabled);
        synchronized (this) {
            if (closed) { decoded.close(); throw new IOException("Stream closed"); }
            decodedBody=decoded;
        }
    }
    private int readRaw(byte[] bytes,int offset,int count) throws Exception {
        try {
            long n=(Long)invoke(api.sourceRead,source,readBuffer,(long)count);
            if (n<0) return -1;
            if (n>count) throw new IOException("HTTP/2 source exceeded read capacity");
            // Okio Buffer.read may stop at a segment boundary. Drain exactly
            // the bytes admitted by Source.read without allocating a byte[].
            int copied=0;
            while (copied<n) {
                int part=(Integer)invoke(api.bufferRead,readBuffer,bytes,offset+copied,(int)n-copied);
                if (part<=0) throw new EOFException("HTTP/2 buffer ended before source bytes");
                copied+=part;
            }
            receivedBytes+=copied;
            return copied;
        } catch (Exception | Error failure) {
            // A failing Source can have appended bytes before throwing. Only
            // this direction's owner clears them; close never races the Buffer.
            discard(readBuffer,failure);
            throw failure;
        }
    }
    int read(byte[] bytes,int offset,int count) throws Exception {
        Objects.checkFromIndexSize(offset,count,bytes.length);
        if (count==0) return 0;
        enter();
        try {
        if (decodedBody==null && !responseEncoding.isEmpty()) initDecodedBody();
        if (decodedBody==null) return readRaw(bytes,offset,count);
        return decodedBody.read(bytes,offset,count);
        } finally { leave(); }
    }
    void write(byte[] bytes,int offset,int count) throws Exception {
        Objects.checkFromIndexSize(offset,count,bytes.length);
        enter();
        try {
            invoke(api.bufferWrite,writeBuffer,bytes,offset,count);
            invoke(api.sinkWrite,sink,writeBuffer,(long)count);
        } catch (Exception | Error failure) {
            discard(writeBuffer,failure);
            throw failure;
        } finally { leave(); }
    }
    private void discard(Object buffer,Throwable failure) {
        try { invoke(api.bufferClear,buffer); }
        catch (Exception | Error cleanup) { failure.addSuppressed(cleanup); }
    }
    void flush(boolean end) throws Exception {
        enter();
        try { if (sink!=null) invoke(end ? api.sinkClose : api.sinkFlush,sink); }
        finally { leave(); }
    }
    @Override public void close() throws IOException {
        Object current;
        Http2SessionPool.Session owned;
        synchronized (this) {
            if (closed) return;
            closed=true; current=stream; owned=lease;
        }
        // Reset only this stream. Its blocked reads and flow-control writes
        // wake without closing another stream's physical connection.
        try { reset(current); }
        finally {
            try { if (owned!=null) owned.cancelConnectingOrWake(); }
            finally { retireIfClosed(); }
        }
    }
}
