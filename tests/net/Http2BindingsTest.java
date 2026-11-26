// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.Network;
import java.io.*;
import java.lang.reflect.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Complete real HTTP/2 streams through the production transport and pinned
 * framing provider. Reflection keeps the fixture valid across ART boot/test
 * loaders. The peer writes independent HPACK literals and decodes requests
 * with the actual provider's separate server-side HPACK state. */
public final class Http2BindingsTest {
    private static final AtomicInteger checks=new AtomicInteger();
    private static void check(boolean condition,String message) {
        checks.incrementAndGet(); if(!condition) throw new AssertionError(message);
    }
    private static Object invoke(Method method,Object target,Object... args) throws Exception {
        try { return method.invoke(target,args); }
        catch(InvocationTargetException failure) {
            Throwable cause=failure.getCause();
            if(cause instanceof Error) throw (Error)cause;
            if(cause instanceof Exception) throw (Exception)cause;
            throw new AssertionError(cause);
        }
    }
    private static Method method(Class<?> type,String name,Class<?>... signature) throws Exception {
        Method result=type.getDeclaredMethod(name,signature); result.setAccessible(true); return result;
    }
    private static Constructor<?> constructor(Class<?> type,Class<?>... signature) throws Exception {
        Constructor<?> result=type.getDeclaredConstructor(signature); result.setAccessible(true); return result;
    }
    private static final class Bridge {
        final Class<?> poolType=Class.forName("android.net.http.Http2SessionPool");
        final Class<?> transportType=Class.forName("android.net.http.Http2Transport");
        final Constructor<?> pool=constructor(poolType), transport=constructor(transportType,poolType);
        final Method connect=method(transportType,"connect",URL.class,Network.class);
        final Method start=method(transportType,"start",URL.class,String.class,List.class,String.class,boolean.class);
        final Method headers=method(transportType,"headers"), read=method(transportType,"read",byte[].class,int.class,int.class);
        Bridge() throws Exception {}
    }
    private static final class HeaderDecoder {
        final Object buffer,reader;
        final Method write,readHeaders,headers,utf8;
        final Field name,value;
        HeaderDecoder() throws Exception {
            Class<?> b=Class.forName("com.android.okhttp.okio.Buffer");
            Class<?> r=Class.forName("com.android.okhttp.internal.framed.Hpack$Reader");
            Class<?> h=Class.forName("com.android.okhttp.internal.framed.Header");
            buffer=b.getConstructor().newInstance();
            reader=constructor(r,int.class,Class.forName("com.android.okhttp.okio.Source")).newInstance(4096,buffer);
            write=b.getMethod("write",byte[].class);
            readHeaders=method(r,"readHeaders"); headers=method(r,"getAndResetHeaderList");
            utf8=Class.forName("com.android.okhttp.okio.ByteString").getMethod("utf8");
            name=h.getField("name"); value=h.getField("value");
        }
        List<Map.Entry<String,String>> decode(byte[] block) throws Exception {
            invoke(write,buffer,(Object)block); invoke(readHeaders,reader);
            List<Map.Entry<String,String>> result=new ArrayList<>();
            for(Object h:(List<?>)invoke(headers,reader)) result.add(new AbstractMap.SimpleImmutableEntry<>(
                    (String)invoke(utf8,name.get(h)),(String)invoke(utf8,value.get(h))));
            return result;
        }
    }
    private static void length(OutputStream out,int value) throws IOException {
        if(value<127) { out.write(value); return; }
        out.write(127); value-=127;
        while(value>=128) { out.write((value&127)|128); value>>>=7; }
        out.write(value);
    }
    private static void literal(OutputStream out,String name,String value) throws IOException {
        out.write(0); // Literal without indexing, new name; no Huffman coding.
        for(String part:new String[]{name,value}) {
            byte[] bytes=part.getBytes(StandardCharsets.UTF_8); length(out,bytes.length); out.write(bytes);
        }
    }
    private static String headerName(int index) { return index%4==0 ? "x-repeat" : "x-field-"+index; }
    private static String headerValue(int index) { return index%7==0 ? "עברית-"+index : "value-"+index; }
    private static void frame(OutputStream out,int type,int flags,int stream,byte[] bytes) throws IOException {
        int size=bytes.length;
        out.write(new byte[]{(byte)(size>>16),(byte)(size>>8),(byte)size,(byte)type,(byte)flags,
                (byte)(stream>>24),(byte)(stream>>16),(byte)(stream>>8),(byte)stream});
        out.write(bytes); out.flush();
    }
    private static final class Peer implements AutoCloseable {
        final ServerSocket listener=new ServerSocket(0,8,InetAddress.getByName("127.0.0.1"));
        final AtomicInteger accepted=new AtomicInteger(), requests=new AtomicInteger(), resets=new AtomicInteger();
        final AtomicReference<Throwable> failure=new AtomicReference<>();
        final List<Thread> workers=Collections.synchronizedList(new ArrayList<>());
        final List<Socket> sockets=Collections.synchronizedList(new ArrayList<>());
        final byte[] commonHeaders;
        final int bodySize;
        final Thread acceptor;
        volatile boolean closing;
        Peer(int headerCount,int bodySize) throws Exception {
            this.bodySize=bodySize;
            ByteArrayOutputStream block=new ByteArrayOutputStream(); block.write(0x88); // :status 200
            for(int i=0;i<headerCount;i++) literal(block,headerName(i),headerValue(i));
            commonHeaders=block.toByteArray();
            acceptor=new Thread(()->{
                try { while(!closing) {
                    Socket socket=listener.accept(); sockets.add(socket); accepted.incrementAndGet();
                    Thread worker=new Thread(()->serve(socket),"http2-bindings-peer"); workers.add(worker); worker.start();
                }} catch(Throwable error) { if(!closing) failure.compareAndSet(null,error); }
            },"http2-bindings-accept");
            acceptor.start();
        }
        URL url() throws Exception { return new URL("http://127.0.0.1:"+listener.getLocalPort()+"/bindings?owned=1"); }
        void serve(Socket socket) {
            try(Socket owned=socket) {
                socket.setTcpNoDelay(true); socket.setSoTimeout(15000);
                HeaderDecoder decoder=new HeaderDecoder();
                DataInputStream in=new DataInputStream(socket.getInputStream()); OutputStream out=socket.getOutputStream();
                byte[] preface=new byte[24]; in.readFully(preface);
                check(Arrays.equals(preface,"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n".getBytes(StandardCharsets.US_ASCII)),"Client preface changed");
                frame(out,4,0,0,new byte[0]);
                long credit=65535; int last=0;
                LinkedHashMap<Integer,byte[]> pending=new LinkedHashMap<>();
                for(;;) {
                    int size=(in.readUnsignedByte()<<16)|(in.readUnsignedByte()<<8)|in.readUnsignedByte();
                    int type=in.readUnsignedByte(),flags=in.readUnsignedByte(),id=in.readInt()&0x7fffffff;
                    check(size<=16384,"Unexpected frame bound"); byte[] bytes=new byte[size]; in.readFully(bytes);
                    if(type==4 && (flags&1)==0) frame(out,4,1,0,new byte[0]);
                    if(type==6 && (flags&1)==0) frame(out,6,1,0,bytes);
                    if(type==8 && id==0) {
                        check(bytes.length==4,"WINDOW_UPDATE length");
                        credit+=((bytes[0]&127L)<<24)|((bytes[1]&255L)<<16)|((bytes[2]&255L)<<8)|(bytes[3]&255L);
                    }
                    if(type==1) {
                        check(id>last && (id&1)==1 && (flags&5)==5,"GET stream ownership/header boundary"); last=id;
                        List<Map.Entry<String,String>> request=decoder.decode(bytes);
                        check(request.contains(new AbstractMap.SimpleImmutableEntry<>(":method","GET")),"Request method changed");
                        check(request.contains(new AbstractMap.SimpleImmutableEntry<>(":path","/bindings?owned=1")),"Request target changed");
                        String owner=null; int repeated=0;
                        for(Map.Entry<String,String> h:request) {
                            if(h.getKey().equals("x-owner")) owner=h.getValue();
                            if(h.getKey().equals("x-request-repeat")) {
                                check(h.getValue().equals(repeated++==0 ? "first" : "second"),"Request duplicate order changed");
                            }
                        }
                        check(owner!=null && repeated==2,"Request headers missing");
                        int ordinal=Integer.parseInt(owner); requests.incrementAndGet();
                        ByteArrayOutputStream response=new ByteArrayOutputStream(); response.write(commonHeaders);
                        literal(response,"x-owner",owner);
                        frame(out,1,bodySize==0 ? 5 : 4,id,response.toByteArray());
                        if(bodySize>0) {
                            byte[] body=new byte[bodySize];
                            for(int i=0;i<body.length;i++) body[i]=(byte)(i*31+ordinal*17);
                            pending.put(id,body);
                        }
                    }
                    if(type==3) resets.incrementAndGet();
                    for(Iterator<Map.Entry<Integer,byte[]>> it=pending.entrySet().iterator();it.hasNext();) {
                        Map.Entry<Integer,byte[]> next=it.next();
                        if(credit<next.getValue().length) break;
                        credit-=next.getValue().length;
                        frame(out,0,1,next.getKey(),next.getValue()); it.remove();
                    }
                }
            } catch(EOFException | SocketException done) {
                // Pool retirement closes the owned connection after all bodies.
            } catch(Throwable error) { failure.compareAndSet(null,error); }
        }
        void healthy() { if(failure.get()!=null) throw new AssertionError("Independent wire peer failed",failure.get()); }
        @Override public void close() throws Exception {
            closing=true; listener.close();
            synchronized(sockets) { for(Socket socket:sockets) socket.close(); }
            acceptor.join(5000); check(!acceptor.isAlive(),"Peer acceptor retained");
            synchronized(workers) { for(Thread worker:workers) { worker.join(5000); check(!worker.isAlive(),"Peer worker retained"); } }
            healthy();
        }
    }
    private static final class Counters {
        final Object thread,process;
        final Method cpu,allocated,processCpu;
        Counters() throws Exception {
            Class<?> factory=Class.forName("java.lang.management.ManagementFactory");
            thread=factory.getMethod("getThreadMXBean").invoke(null);
            process=factory.getMethod("getOperatingSystemMXBean").invoke(null);
            cpu=Class.forName("java.lang.management.ThreadMXBean").getMethod("getCurrentThreadCpuTime");
            Class<?> allocation=Class.forName("com.sun.management.ThreadMXBean");
            allocation.getMethod("setThreadAllocatedMemoryEnabled",boolean.class).invoke(thread,true);
            allocated=allocation.getMethod("getThreadAllocatedBytes",long.class);
            processCpu=Class.forName("com.sun.management.OperatingSystemMXBean").getMethod("getProcessCpuTime");
        }
        long cpu() throws Exception { return (Long)invoke(cpu,thread); }
        long allocated() throws Exception { return (Long)invoke(allocated,thread,Thread.currentThread().getId()); }
        long processCpu() throws Exception { return (Long)invoke(processCpu,process); }
    }
    @SuppressWarnings("unchecked")
    private static void request(Bridge api,Object pool,Peer peer,int ordinal,int headerCount,int bodySize) throws Exception {
        URL url=peer.url(); Object transport=api.transport.newInstance(pool);
        try(Closeable owned=(Closeable)transport) {
            invoke(api.connect,transport,url,null);
            List<Map.Entry<String,String>> headers=Arrays.asList(
                    new AbstractMap.SimpleImmutableEntry<>("X-Owner",Integer.toString(ordinal)),
                    new AbstractMap.SimpleImmutableEntry<>("X-Request-Repeat","first"),
                    new AbstractMap.SimpleImmutableEntry<>("x-request-repeat","second"));
            invoke(api.start,transport,url,"GET",headers,"bindings-contract",false);
            List<Map.Entry<String,String>> response=(List<Map.Entry<String,String>>)invoke(api.headers,transport);
            check(response.size()==headerCount+2,"Response header count changed");
            check(response.get(0).getKey().equals(":status") && response.get(0).getValue().equals("200"),"Status changed");
            for(int i=0;i<headerCount;i++) {
                Map.Entry<String,String> value=response.get(i+1);
                check(value.getKey().equals(headerName(i)) && value.getValue().equals(headerValue(i)),"Response order/UTF-8/duplicates changed");
            }
            check(response.get(headerCount+1).equals(new AbstractMap.SimpleImmutableEntry<>("x-owner",Integer.toString(ordinal))),"Cross-stream header ownership");
            byte[] bytes=new byte[4096]; int total=0;
            for(int count;(count=(Integer)invoke(api.read,transport,bytes,0,bytes.length))!=-1;) {
                check(count>0 && total+count<=bodySize,"Body framing/length changed");
                for(int i=0;i<count;i++) check(bytes[i]==(byte)((total+i)*31+ordinal*17),"Cross-stream body ownership");
                total+=count;
            }
            check(total==bodySize,"Incomplete response body");
        }
        peer.healthy();
    }
    private static void workload(int count,int headers,int body,int warm,boolean measure) throws Exception {
        Bridge api=new Bridge(); Counters counters=measure ? new Counters() : null;
        Peer peer=new Peer(headers,body); Object pool=api.pool.newInstance();
        long beforeAlloc=0,beforeCpu=0,beforeProcess=0,beforeWall=0;
        try {
            for(int i=0;i<warm;i++) request(api,pool,peer,i,headers,body);
            if(measure) {
                beforeAlloc=counters.allocated(); beforeCpu=counters.cpu(); beforeProcess=counters.processCpu(); beforeWall=System.nanoTime();
            }
            for(int i=0;i<count;i++) request(api,pool,peer,warm+i,headers,body);
        } finally { ((Closeable)pool).close(); peer.close(); }
        long elapsed=measure ? System.nanoTime()-beforeWall : -1;
        long process=measure ? counters.processCpu()-beforeProcess : -1;
        long cpu=measure ? counters.cpu()-beforeCpu : -1;
        long allocation=measure ? counters.allocated()-beforeAlloc : -1;
        check(peer.accepted.get()==1 && peer.requests.get()==warm+count,"Requests did not use one actual connection");
        check(peer.resets.get()==0,"Successful requests emitted RST_STREAM");
        if(measure) {
            check(cpu>0 && process>0 && allocation>0,"Missing measurement cannot be reported as zero");
            System.out.println("HTTP2_BINDINGS_WORK requests="+count+" headers="+headers+" body="+body+" warm="+warm
                    +" callerCpuNs="+cpu+" callerAllocatedBytes="+allocation+" processCpuNs="+process+" elapsedNs="+elapsed
                    +" connections="+peer.accepted.get()+" completeBytes="+((long)count*body));
        }
    }
    public static void main(String[] args) throws Exception {
        if(args.length>0 && args[0].equals("--workload")) {
            workload(Integer.parseInt(args[1]),Integer.parseInt(args[2]),Integer.parseInt(args[3]),Integer.parseInt(args[4]),true);
        } else {
            for(int headers:new int[]{0,16,64,256}) workload(12,headers,headers==256 ? 0 : 4093,0,false);
        }
        System.out.println("Http2BindingsTest passed: "+checks.get()+" actual-provider/wire checks");
    }
}
