// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import android.net.Network;
import java.io.*;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.*;
import java.util.*;
import javax.net.ssl.*;
import java.util.zip.GZIPInputStream;
import java.util.zip.InflaterInputStream;

/** A narrow adapter to AOSP OkHttp's HTTP/2 framing already in core-libart.jar.
 * Like Network's HttpURLConnectionFactory adapter, reflection keeps this out of
 * the framework compile-only class path; no app-supplied Cronet code is loaded.
 */
final class Http2Transport implements Closeable {
    private static final String HTTP="com.android.okhttp.";
    private static final String IO=HTTP+"okio.";
    private Socket socket;
    private Object connection, stream, source, sink;
    private boolean closed;
    private volatile InputStream decodedBody;
    private String responseEncoding="";
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
    private final Method bufferRead=bufferClass.getMethod("read",byte[].class,int.class,int.class);
    private final Method bufferWrite=bufferClass.getMethod("write",byte[].class,int.class,int.class);
    private final Method bufferClear=bufferClass.getMethod("clear");
    private final Method sinkWrite=sinkClass.getMethod("write",bufferClass,long.class);
    private final Method sinkFlush=sinkClass.getMethod("flush");
    private final Method sinkClose=sinkClass.getMethod("close");

    Http2Transport() throws ReflectiveOperationException {}
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
    private synchronized void retain(Socket value) throws IOException {
        if (closed) { value.close(); throw new IOException("Stream closed"); }
        socket=value;
    }
    static String authority(URL url, boolean includeDefaultPort) {
        String host=url.getHost();
        if (host.contains(":") && !host.startsWith("[")) host="["+host+"]";
        int port=url.getPort();
        if (port<0 && includeDefaultPort) port=url.getDefaultPort();
        return port<0 ? host : host+":"+port;
    }
    void connect(URL url, Network network) throws Exception {
        int port=url.getPort()<0 ? url.getDefaultPort() : url.getPort();
        boolean tls=url.getProtocol().equals("https");
        if (!tls) {
            Class<?> policy=Class.forName("android.security.NetworkSecurityPolicy");
            Object instance=call(policy,null,"getInstance",new Class<?>[0]);
            if (!(Boolean)call(policy,instance,"isCleartextTrafficPermitted",new Class<?>[]{String.class},url.getHost()))
                throw new IOException("Cleartext traffic is not permitted");
        }
        List<Proxy> proxies=ProxySelector.getDefault()==null ? Collections.singletonList(Proxy.NO_PROXY)
                : ProxySelector.getDefault().select(url.toURI());
        Proxy proxy=proxies==null || proxies.isEmpty() ? Proxy.NO_PROXY : proxies.get(0);
        if (proxy.type()==Proxy.Type.SOCKS && network!=null)
            throw new IOException("SOCKS with explicit network binding is unsupported");
        Socket raw=network!=null ? network.getSocketFactory().createSocket() : new Socket(proxy.type()==Proxy.Type.SOCKS ? proxy : Proxy.NO_PROXY);
        retain(raw); raw.setSoTimeout(30000);
        InetSocketAddress address=proxy.type()==Proxy.Type.HTTP ? (InetSocketAddress)proxy.address()
                : proxy.type()==Proxy.Type.SOCKS ? InetSocketAddress.createUnresolved(url.getHost(),port)
                : new InetSocketAddress(network==null ? InetAddress.getByName(url.getHost()) : network.getByName(url.getHost()),port);
        // Android's ProxySelector returns unresolved HTTP-proxy addresses even
        // for loopback. Plain Socket.connect requires a resolved address; DNS
        // for the origin still belongs to the CONNECT proxy, not the host.
        if (proxy.type()==Proxy.Type.HTTP && address.isUnresolved()) address=new InetSocketAddress(
                network==null ? InetAddress.getByName(address.getHostString()) : network.getByName(address.getHostString()),address.getPort());
        raw.connect(address,30000);
        if (proxy.type()==Proxy.Type.HTTP) {
            if (!tls) throw new IOException("Cleartext HTTP/2 over HTTP proxy is unsupported");
            String authority=authority(url,true);
            OutputStream out=raw.getOutputStream();
            out.write(("CONNECT "+authority+" HTTP/1.1\r\nHost: "+authority+"\r\n\r\n").getBytes("US-ASCII")); out.flush();
            InputStream in=raw.getInputStream();
            String status=line(in);
            if (!status.matches("HTTP/1\\.[01] 200(?: .*|)")) throw new IOException("HTTP proxy refused CONNECT");
            int length=status.length();
            for (String header;!(header=line(in)).isEmpty();) {
                length+=header.length(); if (length>65536) throw new IOException("Proxy response headers too large");
            }
        }
        if (tls) {
            SSLSocket ssl=(SSLSocket)HttpsURLConnection.getDefaultSSLSocketFactory().createSocket(raw,url.getHost(),port,true);
            retain(ssl);
            SSLParameters parameters=ssl.getSSLParameters();
            parameters.setEndpointIdentificationAlgorithm("HTTPS");
            parameters.setApplicationProtocols(new String[]{"h2"});
            ssl.setSSLParameters(parameters); ssl.startHandshake();
            if (!"h2".equals(ssl.getApplicationProtocol())) throw new SSLException("Server did not negotiate HTTP/2");
        }
        Class<?> builderClass=Class.forName(HTTP+"internal.framed.FramedConnection$Builder");
        Class<?> protocolClass=Class.forName(HTTP+"Protocol");
        Object builder=builderClass.getConstructor(boolean.class).newInstance(true);
        call(builderClass,builder,"socket",new Class<?>[]{Socket.class},socket);
        call(builderClass,builder,"protocol",new Class<?>[]{protocolClass},protocolClass.getField("HTTP_2").get(null));
        Object opened=call(builderClass,builder,"build",new Class<?>[0]);
        synchronized (this) {
            if (closed) { ((Closeable)opened).close(); throw new IOException("Stream closed"); }
            connection=opened;
        }
        call(opened.getClass(),opened,"sendConnectionPreface",new Class<?>[0]);
    }
    private static String line(InputStream in) throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        for (int ch;(ch=in.read())!=-1;) {
            if (ch=='\n') return bytes.toString("US-ASCII").replace("\r","");
            bytes.write(ch); if (bytes.size()>8192) throw new IOException("Proxy response line too long");
        }
        throw new EOFException("Proxy closed CONNECT response");
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
        stream=call(connection.getClass(),connection,"newStream",new Class<?>[]{List.class,boolean.class,boolean.class},encoded,hasBody,true);
        source=call(streamClass,stream,"getSource",new Class<?>[0]);
        if (hasBody) sink=call(streamClass,stream,"getSink",new Class<?>[0]);
        call(connection.getClass(),connection,"flush",new Class<?>[0]);
    }
    List<Map.Entry<String,String>> headers() throws Exception {
        List<?> headers=(List<?>)call(streamClass,stream,"getResponseHeaders",new Class<?>[0]);
        List<Map.Entry<String,String>> result=new ArrayList<>();
        for (Object h:headers) {
            Object name=h.getClass().getField("name").get(h), value=h.getClass().getField("value").get(h);
            result.add(new AbstractMap.SimpleImmutableEntry<>((String)call(name.getClass(),name,"utf8",new Class<?>[0]),
                    (String)call(value.getClass(),value,"utf8",new Class<?>[0])));
        }
        return result;
    }
    void setResponseEncoding(String encoding) throws IOException {
        if (!encoding.isEmpty() && !encoding.equalsIgnoreCase("identity")
                && !encoding.equalsIgnoreCase("gzip") && !encoding.equalsIgnoreCase("deflate"))
            throw new IOException("Unsupported HTTP content encoding");
        responseEncoding=encoding;
    }
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
        // GZIPInputStream reads its header in the constructor. Do that only in
        // the body pump, after the response-header callback has been dispatched:
        // a duplex peer may wait for a write from that callback before its body.
        InputStream decoded=responseEncoding.equalsIgnoreCase("gzip") ? new GZIPInputStream(raw)
                : new InflaterInputStream(raw);
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
        if (decodedBody==null && (responseEncoding.equalsIgnoreCase("gzip")
                || responseEncoding.equalsIgnoreCase("deflate"))) initDecodedBody();
        if (decodedBody==null) return readRaw(bytes,offset,count);
        return decodedBody.read(bytes,offset,count);
    }
    void write(byte[] bytes,int offset,int count) throws Exception {
        Objects.checkFromIndexSize(offset,count,bytes.length);
        try {
            invoke(bufferWrite,writeBuffer,bytes,offset,count);
            invoke(sinkWrite,sink,writeBuffer,(long)count);
        } catch (Exception | Error failure) {
            discard(writeBuffer,failure);
            throw failure;
        }
    }
    private void discard(Object buffer,Throwable failure) {
        try { invoke(bufferClear,buffer); }
        catch (Exception | Error cleanup) { failure.addSuppressed(cleanup); }
    }
    void flush(boolean end) throws Exception {
        if (sink!=null) invoke(end ? sinkClose : sinkFlush,sink);
    }
    @Override public synchronized void close() throws IOException {
        closed=true;
        // Closing the socket first wakes blocked HTTP/2 reads and flow-control writers.
        try { if (socket!=null) socket.close(); }
        finally {
            try { if (decodedBody!=null) decodedBody.close(); }
            finally { if (connection!=null) ((Closeable)connection).close(); }
        }
    }
}
