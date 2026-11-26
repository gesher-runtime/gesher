// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.net.Socket;
import java.util.List;
import java.util.concurrent.TimeUnit;

/** Fixed bindings to the runtime's pinned HTTP/2 provider, never to app code.
 * Only class/member metadata and immutable enum values are shared. Buffers,
 * sockets, streams, routes and policy answers keep their existing owners. */
final class Http2Bindings {
    private static volatile Http2Bindings resolved;

    static Http2Bindings get() throws ReflectiveOperationException {
        Http2Bindings value=resolved;
        if(value!=null) return value;
        synchronized(Http2Bindings.class) {
            value=resolved;
            // Publish only a complete binding set. A failed lookup must retain
            // the original reflective failure, not poison a static initializer.
            if(value==null) resolved=value=new Http2Bindings();
            return value;
        }
    }

    final Constructor<?> buffer, header, connectionBuilder;
    final Method sourceRead, sourceClose, bufferRead, bufferWrite, bufferClear;
    final Method sinkWrite, sinkFlush, sinkClose, sinkTimeout, timeout;
    final Method streamIsOpen, streamCloseLater, streamSource, streamSink;
    final Method streamReadTimeout, streamWriteTimeout, streamResponseHeaders;
    final Method connectionMaximumStreams, connectionOpenStreams, connectionNewStream;
    final Method connectionFlush, connectionPreface;
    final Method socketSource, socketSink, bufferedSource, bufferedSink;
    final Method builderSocket, builderProtocol, builderBuild, bytesUtf8;
    final Method policyInstance, cleartextPermitted;
    final Field headerName, headerValue, connectionShutdown, connectionNextStreamId;
    final Object cancel, http2;

    private Http2Bindings() throws ReflectiveOperationException {
        String http="com.android.okhttp.", io=http+"okio.", framed=http+"internal.framed.";
        Class<?> bufferType=Class.forName(io+"Buffer");
        Class<?> sourceType=Class.forName(io+"Source"), sinkType=Class.forName(io+"Sink");
        Class<?> stream=Class.forName(framed+"FramedStream");
        Class<?> connection=Class.forName(framed+"FramedConnection");
        Class<?> builder=Class.forName(framed+"FramedConnection$Builder");
        Class<?> headerType=Class.forName(framed+"Header");
        Class<?> error=Class.forName(framed+"ErrorCode");
        Class<?> okio=Class.forName(io+"Okio"), protocol=Class.forName(http+"Protocol");
        Class<?> policy=Class.forName("android.security.NetworkSecurityPolicy");
        buffer=bufferType.getConstructor();
        header=headerType.getConstructor(String.class,String.class);
        connectionBuilder=builder.getConstructor(boolean.class);
        sourceRead=sourceType.getMethod("read",bufferType,long.class);
        sourceClose=sourceType.getMethod("close");
        bufferRead=bufferType.getMethod("read",byte[].class,int.class,int.class);
        bufferWrite=bufferType.getMethod("write",byte[].class,int.class,int.class);
        bufferClear=bufferType.getMethod("clear");
        sinkWrite=sinkType.getMethod("write",bufferType,long.class);
        sinkFlush=sinkType.getMethod("flush");
        sinkClose=sinkType.getMethod("close");
        sinkTimeout=sinkType.getMethod("timeout");
        timeout=Class.forName(io+"Timeout").getMethod("timeout",long.class,TimeUnit.class);
        streamIsOpen=stream.getMethod("isOpen");
        streamCloseLater=stream.getMethod("closeLater",error);
        streamSource=stream.getMethod("getSource");
        streamSink=stream.getMethod("getSink");
        streamReadTimeout=stream.getMethod("readTimeout");
        streamWriteTimeout=stream.getMethod("writeTimeout");
        streamResponseHeaders=stream.getMethod("getResponseHeaders");
        connectionMaximumStreams=connection.getMethod("maxConcurrentStreams");
        connectionOpenStreams=connection.getMethod("openStreamCount");
        connectionNewStream=connection.getMethod("newStream",List.class,boolean.class,boolean.class);
        connectionFlush=connection.getMethod("flush");
        connectionPreface=connection.getMethod("sendConnectionPreface");
        socketSource=okio.getMethod("source",Socket.class);
        socketSink=okio.getMethod("sink",Socket.class);
        bufferedSource=okio.getMethod("buffer",sourceType);
        bufferedSink=okio.getMethod("buffer",sinkType);
        builderSocket=builder.getMethod("socket",Socket.class,String.class,
                Class.forName(io+"BufferedSource"),Class.forName(io+"BufferedSink"));
        builderProtocol=builder.getMethod("protocol",protocol);
        builderBuild=builder.getMethod("build");
        bytesUtf8=Class.forName(io+"ByteString").getMethod("utf8");
        policyInstance=policy.getMethod("getInstance");
        cleartextPermitted=policy.getMethod("isCleartextTrafficPermitted",String.class);
        headerName=headerType.getField("name");
        headerValue=headerType.getField("value");
        connectionShutdown=connection.getDeclaredField("shutdown");
        connectionShutdown.setAccessible(true);
        connectionNextStreamId=connection.getDeclaredField("nextStreamId");
        connectionNextStreamId.setAccessible(true);
        cancel=error.getField("CANCEL").get(null);
        http2=protocol.getField("HTTP_2").get(null);
    }
}
