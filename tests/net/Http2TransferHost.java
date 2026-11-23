// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import com.android.okhttp.okio.Buffer;
import com.android.okhttp.okio.Source;
import com.android.okhttp.okio.Sink;
import com.android.okhttp.okio.Timeout;
import java.io.*;
import java.lang.management.ManagementFactory;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;
import java.util.zip.*;

/** Real production adapter and pinned AOSP Okio, with deterministic source/sink
 * boundaries. Wire, TLS, callback and policy checks run in the companion tests.
 * The same fixture accepts the old internal API for measured baseline runs. */
public final class Http2TransferHost {
    static int checks;
    static final Method READ, WRITE;
    static final boolean REUSABLE;
    static {
        try {
            Method read, write;
            boolean reusable;
            try {
                read=Http2Transport.class.getDeclaredMethod("read",byte[].class,int.class,int.class);
                write=Http2Transport.class.getDeclaredMethod("write",byte[].class,int.class,int.class);
                reusable=true;
            } catch (NoSuchMethodException old) {
                read=Http2Transport.class.getDeclaredMethod("read",int.class);
                write=Http2Transport.class.getDeclaredMethod("write",byte[].class);
                reusable=false;
            }
            READ=read; WRITE=write; REUSABLE=reusable;
        } catch (ReflectiveOperationException e) { throw new ExceptionInInitializerError(e); }
    }
    static void check(boolean value,String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    static Object invoke(Method method,Object target,Object... args) throws Exception {
        try { return method.invoke(target,args); }
        catch (InvocationTargetException e) {
            if (e.getCause() instanceof Exception) throw (Exception)e.getCause();
            throw (Error)e.getCause();
        }
    }
    static void set(Http2Transport transport,String name,Object value) throws Exception {
        Field field=Http2Transport.class.getDeclaredField(name);
        field.setAccessible(true); field.set(transport,value);
    }
    static int read(Http2Transport transport,byte[] bytes,int offset,int count) throws Exception {
        if (REUSABLE) return (Integer)invoke(READ,transport,bytes,offset,count);
        byte[] part=(byte[])invoke(READ,transport,count);
        if (part==null) return -1;
        System.arraycopy(part,0,bytes,offset,part.length);
        return part.length;
    }
    static void write(Http2Transport transport,byte[] bytes,int offset,int count) throws Exception {
        if (REUSABLE) invoke(WRITE,transport,bytes,offset,count);
        else invoke(WRITE,transport,(Object)(offset==0 && count==bytes.length
                ? bytes : Arrays.copyOfRange(bytes,offset,offset+count)));
    }
    static byte[] pattern(int length) {
        byte[] bytes=new byte[length]; new Random(7019).nextBytes(bytes); return bytes;
    }
    static byte[] compressed(byte[] bytes,String encoding) throws IOException {
        ByteArrayOutputStream output=new ByteArrayOutputStream();
        try (OutputStream compressor=encoding.equals("gzip") ? new GZIPOutputStream(output)
                : new DeflaterOutputStream(output)) { compressor.write(bytes); }
        return output.toByteArray();
    }
    static byte[] drain(Http2Transport transport) throws Exception {
        ByteArrayOutputStream output=new ByteArrayOutputStream();
        byte[] scratch=new byte[32776]; int[] sizes={1,17,8191,8192,8193,32768}; int index=0;
        for (;;) {
            Arrays.fill(scratch,(byte)0x55);
            int n=read(transport,scratch,3,sizes[index++%sizes.length]);
            check(scratch[0]==0x55 && scratch[2]==0x55 && scratch[scratch.length-1]==0x55,
                    "read overwrote bytes outside the supplied range");
            if (n<0) break;
            check(n>0,"positive read made no progress");
            output.write(scratch,3,n);
        }
        return output.toByteArray();
    }
    static void contracts() throws Exception {
        byte[] input=pattern(100_019);
        for (String encoding:new String[]{"identity","gzip","deflate"}) {
            try (Http2Transport transport=new Http2Transport()) {
                byte[] encoded=encoding.equals("identity") ? input : compressed(input,encoding);
                set(transport,"source",new Buffer().write(encoded));
                transport.setResponseEncoding(encoding);
                check(Arrays.equals(input,drain(transport)),"binary/segment/decompression mismatch: "+encoding);
                check(read(transport,new byte[3],0,3)==-1,"EOF must stay EOF");
            }
        }
        try (Http2Transport transport=new Http2Transport()) {
            IOException failure=new IOException("source appended before failure");
            set(transport,"source",new Source() {
                public long read(Buffer sink,long count) throws IOException { sink.writeUtf8("stale"); throw failure; }
                public Timeout timeout() { return Timeout.NONE; }
                public void close() {}
            });
            try { read(transport,new byte[99],0,99); throw new AssertionError("missing read failure"); }
            catch (IOException e) { check(e==failure,"source failure identity changed"); }
            set(transport,"source",new Buffer().writeUtf8("fresh"));
            check(Arrays.equals("fresh".getBytes("US-ASCII"),drain(transport)),"failed read retained stale bytes");
            set(transport,"sink",new Sink() {
                public void write(Buffer source,long count) throws IOException { source.skip(count/2); throw failure; }
                public void flush() {} public void close() {} public Timeout timeout() { return Timeout.NONE; }
            });
            try { write(transport,input,7,32768); throw new AssertionError("missing write failure"); }
            catch (IOException e) { check(e==failure,"sink failure identity changed"); }
            Buffer written=new Buffer(); set(transport,"sink",written);
            write(transport,input,11,9001); write(transport,input,3,17);
            ByteArrayOutputStream expected=new ByteArrayOutputStream();
            expected.write(input,11,9001); expected.write(input,3,17);
            check(Arrays.equals(expected.toByteArray(),written.readByteArray()),"write range or failure cleanup mismatch");
            if (REUSABLE) {
                set(transport,"source",new Buffer());
                transport.setResponseEncoding("gzip");
                check(read(transport,new byte[1],1,0)==0,"zero read started decompression");
                try { read(transport,new byte[8],7,2); throw new AssertionError("missing read bounds"); }
                catch (IndexOutOfBoundsException expectedError) { checks++; }
                try { write(transport,new byte[8],-1,1); throw new AssertionError("missing write bounds"); }
                catch (IndexOutOfBoundsException expectedError) { checks++; }
            }
        }
        for (String encoding:new String[]{"gzip","deflate"}) {
            try (Http2Transport transport=new Http2Transport()) {
                byte[] encoded=compressed(input,encoding);
                set(transport,"source",new Buffer().write(encoded,0,encoded.length-7));
                transport.setResponseEncoding(encoding);
                try { drain(transport); throw new AssertionError("truncated compression accepted"); }
                catch (IOException expected) { checks++; }
            }
        }
        duplex();
    }
    static void duplex() throws Exception {
        CountDownLatch readerEntered=new CountDownLatch(1),writerFinished=new CountDownLatch(1);
        AtomicReference<Throwable> failure=new AtomicReference<>();
        AtomicReference<Buffer> readerBuffer=new AtomicReference<>();
        try (Http2Transport transport=new Http2Transport()) {
            set(transport,"source",new Source() {
                public long read(Buffer sink,long count) throws IOException {
                    readerBuffer.set(sink); readerEntered.countDown();
                    try { if (!writerFinished.await(5,TimeUnit.SECONDS)) throw new IOException("duplex writer blocked"); }
                    catch (InterruptedException e) { throw new IOException(e); }
                    sink.writeUtf8("read"); return 4;
                }
                public Timeout timeout() { return Timeout.NONE; } public void close() {}
            });
            set(transport,"sink",new Sink() {
                public void write(Buffer source,long count) throws IOException {
                    check(source!=readerBuffer.get(),"read and write shared a mutable Okio buffer");
                    check(source.readUtf8(count).equals("write"),"duplex writer payload corrupted");
                    writerFinished.countDown();
                }
                public void flush() {} public void close() {} public Timeout timeout() { return Timeout.NONE; }
            });
            Thread reader=new Thread(() -> {
                try {
                    byte[] bytes=new byte[4];
                    check(read(transport,bytes,0,4)==4 && Arrays.equals(bytes,"read".getBytes("US-ASCII")),
                            "duplex reader payload corrupted");
                } catch (Throwable e) { failure.set(e); }
            });
            reader.start();
            try {
                check(readerEntered.await(5,TimeUnit.SECONDS),"reader did not enter");
                write(transport,"write".getBytes("US-ASCII"),0,5);
            } finally { writerFinished.countDown(); reader.join(6000); }
            check(!reader.isAlive(),"reader did not retire");
            if (failure.get()!=null) throw new AssertionError("duplex failure",failure.get());
        }
    }
    static final class RepeatingSource implements Source {
        final byte[] bytes; long remaining;
        RepeatingSource(byte[] bytes,long remaining) { this.bytes=bytes; this.remaining=remaining; }
        public long read(Buffer sink,long count) {
            if (remaining==0) return -1;
            int n=(int)Math.min(Math.min(count,bytes.length),remaining);
            sink.write(bytes,0,n); remaining-=n; return n;
        }
        public Timeout timeout() { return Timeout.NONE; } public void close() {}
    }
    static final class DiscardSink implements Sink {
        long total,checksum;
        public void write(Buffer source,long count) throws IOException {
            if (count>0) { checksum+=source.getByte(0)&255; checksum+=source.getByte(count-1)&255; }
            source.skip(count); total+=count;
        }
        public Timeout timeout() { return Timeout.NONE; } public void flush() {} public void close() {}
    }
    static long exercise(Http2Transport transport,byte[] scratch,long bytes,boolean writing) throws Exception {
        long checksum=0,processed=0;
        if (writing) {
            for (;processed<bytes;processed+=scratch.length) write(transport,scratch,0,scratch.length);
        } else {
            for (;;) {
                // Observe each API's output directly. The baseline must not pay
                // an adapter copy into scratch that production did not require.
                byte[] received=REUSABLE ? scratch : (byte[])invoke(READ,transport,scratch.length);
                int n=REUSABLE ? read(transport,scratch,0,scratch.length)
                        : received==null ? -1 : received.length;
                if (n<0) break;
                if (n==0) throw new AssertionError("benchmark made no progress");
                processed+=n; checksum+=(received[0]&255)+(received[n-1]&255);
            }
        }
        check(processed==bytes,"benchmark changed byte count");
        return checksum;
    }
    static void measure(boolean writing) throws Exception {
        long bytes=64L*1024*1024;
        byte[] block=pattern(32768),scratch=block.clone();
        com.sun.management.ThreadMXBean bean=(com.sun.management.ThreadMXBean)ManagementFactory.getThreadMXBean();
        check(bean.isThreadAllocatedMemorySupported(),"allocation measurement unavailable");
        bean.setThreadAllocatedMemoryEnabled(true);
        bean.setThreadCpuTimeEnabled(true);
        try (Http2Transport transport=new Http2Transport()) {
            DiscardSink sink=new DiscardSink();
            set(transport,"source",new RepeatingSource(block,bytes)); set(transport,"sink",sink);
            exercise(transport,scratch,bytes,writing); // JIT/segment-pool warmup outside measured range.
            set(transport,"source",new RepeatingSource(block,bytes)); sink.total=sink.checksum=0;
            long thread=Thread.currentThread().getId();
            long allocated=bean.getThreadAllocatedBytes(thread),cpu=bean.getCurrentThreadCpuTime(),start=System.nanoTime();
            long checksum=exercise(transport,scratch,bytes,writing);
            long elapsed=System.nanoTime()-start;
            cpu=bean.getCurrentThreadCpuTime()-cpu; allocated=bean.getThreadAllocatedBytes(thread)-allocated;
            if (writing) { check(sink.total==bytes,"sink byte count"); checksum=sink.checksum; }
            long expected=(bytes/block.length)*((block[0]&255)+(block[block.length-1]&255));
            check(checksum==expected,"benchmark payload checksum");
            System.out.println("HTTP2_TRANSFER {\"reusable\":"+REUSABLE+",\"direction\":\""+(writing?"write":"read")
                    +"\",\"bytes\":"+bytes+",\"allocated_bytes\":"+allocated+",\"cpu_ns\":"+cpu
                    +",\"elapsed_ns\":"+elapsed+",\"checksum\":"+checksum+"}");
            // Timing is informational. Each bound excludes the measured original
            // path (about 65 MiB/read and 1.2 MiB/write) with ample candidate margin.
            if (REUSABLE) check(allocated<(writing?1L:2L)*1024*1024,"per-chunk allocation regression");
        }
    }
    public static void main(String[] args) throws Exception {
        contracts(); measure(false); measure(true);
        System.out.println("Http2TransferHost passed: "+checks+" checks; "+(REUSABLE?"candidate":"baseline"));
    }
}
