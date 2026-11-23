// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import android.net.Network;
import java.io.*;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.*;
import java.util.*;

/** A narrow adapter to AOSP OkHttp's HTTP/2 framing already in core-libart.jar.
 * Like Network's HttpURLConnectionFactory adapter, reflection keeps this out of
 * the framework compile-only class path; no app-supplied Cronet code is loaded.
 */
final class Http2Transport implements Closeable {
    private static final String HTTP="com.android.okhttp.";
    private static final String IO=HTTP+"okio.";
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
    private final Class<?> bufferClass=Class.forName(IO+"Buffer");
    private final Class<?> sourceClass=Class.forName(IO+"Source");
    private final Class<?> sinkClass=Class.forName(IO+"Sink");
    private final Class<?> streamClass=Class.forName(HTTP+"internal.framed.FramedStream");
    // The reader and serialized writer are independent owners. Never share a
    // scratch Buffer between them: HTTP/2 can read while a flow-controlled write
    // is blocked. Fixed methods are resolved once per transport, not per chunk.
    private final Object readBuffer=bufferClass.getConstructor().newInstance();
    private final Object writeBuffer=bufferClass.getConstructor().newInstance();
    private final Method sourceRead=sourceClass.getMethod("read",bufferClass,long.class);
    private final Method sourceClose=sourceClass.getMethod("close");
    private final Method bufferRead=bufferClass.getMethod("read",byte[].class,int.class,int.class);
    private final Method bufferWrite=bufferClass.getMethod("write",byte[].class,int.class,int.class);
    private final Method bufferClear=bufferClass.getMethod("clear");
    private final Method sinkWrite=sinkClass.getMethod("write",bufferClass,long.class);
    private final Method sinkFlush=sinkClass.getMethod("flush");
    private final Method sinkClose=sinkClass.getMethod("close");

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
        try { if (inbound!=null) invoke(sourceClose,inbound); } catch (Exception ignored) { }
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
            if (!(Boolean)call(streamClass,value,"isOpen",new Class<?>[0])) return;
            Class<?> error=Class.forName(HTTP+"internal.framed.ErrorCode");
            call(streamClass,value,"closeLater",new Class<?>[]{error},error.getField("CANCEL").get(null));
        } catch (Exception failure) { throw new IOException("Cannot retire HTTP/2 stream",failure); }
    }
    static Object call(Class<?> type, Object target, String name, Class<?>[] signature, Object... args) throws Exception {
        return invoke(type.getMethod(name,signature),target,args);
    }
    private static Object invoke(Method method, Object target, Object... args) throws Exception {
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
        Class<?> headerClass=Class.forName(HTTP+"internal.framed.Header");
        Constructor<?> constructor=headerClass.getConstructor(String.class,String.class);
        List<Object> encoded=new ArrayList<>();
        String host=authority(url,false);
        encoded.add(constructor.newInstance(":method",method));
        encoded.add(constructor.newInstance(":scheme",url.getProtocol()));
        encoded.add(constructor.newInstance(":authority",host));
        String path=url.getFile();
        if (path.isEmpty() || path.charAt(0)=='?') path="/"+path;
        encoded.add(constructor.newInstance(":path",path));
        boolean hasAgent=false;
        for (Map.Entry<String,String> h:headers) {
            String name=h.getKey().toLowerCase(Locale.ROOT);
            if (name.equals("connection") || name.equals("host") || name.equals("keep-alive")
                    || name.equals("transfer-encoding") || name.equals("upgrade") || name.equals("proxy-connection")) continue;
            if (name.equals("user-agent")) hasAgent=true;
            encoded.add(constructor.newInstance(name,h.getValue()));
        }
        if (!hasAgent) encoded.add(constructor.newInstance("user-agent",agent));
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
            Object inbound=call(streamClass,opened,"getSource",new Class<?>[0]);
            synchronized (this) {
                stream=opened; source=inbound;
                if (closed) { reset(opened); throw new IOException("Stream closed"); }
            }
            for (String direction:new String[]{"readTimeout","writeTimeout"}) {
                Object timeout=call(streamClass,opened,direction,new Class<?>[0]);
                call(Class.forName(IO+"Timeout"),timeout,"timeout",
                        new Class<?>[]{long.class,java.util.concurrent.TimeUnit.class},30L,
                        java.util.concurrent.TimeUnit.SECONDS);
            }
            if (hasBody) sink=call(streamClass,opened,"getSink",new Class<?>[0]);
            call(connection.getClass(),connection,"flush",new Class<?>[0]);
        } finally { leave(); }
    }
    List<Map.Entry<String,String>> headers() throws Exception {
        enter();
        try {
        List<?> headers=(List<?>)call(streamClass,stream,"getResponseHeaders",new Class<?>[0]);
        List<Map.Entry<String,String>> result=new ArrayList<>();
        for (Object h:headers) {
            Object name=h.getClass().getField("name").get(h), value=h.getClass().getField("value").get(h);
            result.add(new AbstractMap.SimpleImmutableEntry<>((String)call(name.getClass(),name,"utf8",new Class<?>[0]),
                    (String)call(value.getClass(),value,"utf8",new Class<?>[0])));
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
            long n=(Long)invoke(sourceRead,source,readBuffer,(long)count);
            if (n<0) return -1;
            if (n>count) throw new IOException("HTTP/2 source exceeded read capacity");
            // Okio Buffer.read may stop at a segment boundary. Drain exactly
            // the bytes admitted by Source.read without allocating a byte[].
            int copied=0;
            while (copied<n) {
                int part=(Integer)invoke(bufferRead,readBuffer,bytes,offset+copied,(int)n-copied);
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
            invoke(bufferWrite,writeBuffer,bytes,offset,count);
            invoke(sinkWrite,sink,writeBuffer,(long)count);
        } catch (Exception | Error failure) {
            discard(writeBuffer,failure);
            throw failure;
        } finally { leave(); }
    }
    private void discard(Object buffer,Throwable failure) {
        try { invoke(bufferClear,buffer); }
        catch (Exception | Error cleanup) { failure.addSuppressed(cleanup); }
    }
    void flush(boolean end) throws Exception {
        enter();
        try { if (sink!=null) invoke(end ? sinkClose : sinkFlush,sink); }
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
