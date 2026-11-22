// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0

import com.android.okhttp.Protocol;
import com.android.okhttp.internal.framed.ErrorCode;
import com.android.okhttp.internal.framed.FrameReader;
import com.android.okhttp.internal.framed.FramedConnection;
import com.android.okhttp.internal.framed.FramedStream;
import com.android.okhttp.internal.framed.Header;
import com.android.okhttp.internal.framed.Http2;
import com.android.okhttp.internal.framed.Settings;
import com.android.okhttp.okio.Buffer;
import com.android.okhttp.okio.BufferedSource;
import com.android.okhttp.okio.Okio;
import com.android.okhttp.okio.Sink;
import com.android.okhttp.okio.Source;
import com.android.okhttp.okio.Timeout;
import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.Socket;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Bounded in-memory contracts for the actual pinned framing provider. There is
 * no network peer, socket connection, or large-window exhaustion experiment.
 * Actual FramedConnection/Stream and HTTP/2 reader/writer code own all state.
 * The declared boundaries are a parked input Source and a recording output Sink.
 * Reflection only reaches pre-existing internal entry points, so unchanged
 * provider controls compile and execute this same test on host and ART.
 */
public final class Http2CreditContracts {
    private static int checks;
    private static int failures;

    private static void check(boolean condition, String name) {
        checks++;
        if (!condition) { failures++; System.err.println("FAIL credit: " + name); }
    }
    private static Object field(Object owner, String name) throws Exception {
        Field f=owner.getClass().getDeclaredField(name); f.setAccessible(true); return f.get(owner);
    }
    private static Object call(Object owner, String name, Class<?>[] types, Object... args) throws Exception {
        Method m=owner.getClass().getDeclaredMethod(name,types); m.setAccessible(true);
        try { return m.invoke(owner,args); }
        catch (InvocationTargetException wrapped) {
            Throwable cause=wrapped.getCause();
            if (cause instanceof Exception) throw (Exception)cause;
            throw (Error)cause;
        }
    }
    private static Buffer data(int size) { return new Buffer().write(new byte[size]); }
    private static void receive(FramedStream stream, BufferedSource input, int size) throws Exception {
        call(stream,"receiveData",new Class<?>[]{BufferedSource.class,int.class},input,size);
    }
    private static void fin(FramedStream stream) throws Exception {
        call(stream,"receiveFin",new Class<?>[0]);
    }

    private static final class ParkedSource implements Source {
        boolean closed;
        volatile Thread reader;
        final CountDownLatch entered=new CountDownLatch(1);
        public synchronized long read(Buffer sink,long count) throws IOException {
            reader=Thread.currentThread(); entered.countDown();
            long until=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
            while (!closed) {
                long remaining=until-System.nanoTime();
                if (remaining<=0) throw new IOException("bounded component input deadline");
                try { TimeUnit.NANOSECONDS.timedWait(this,remaining); }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException(e); }
            }
            return -1;
        }
        public Timeout timeout() { return Timeout.NONE; }
        public synchronized void close() { closed=true; notifyAll(); }
    }
    private static final class RecordedSink implements Sink {
        final Buffer bytes=new Buffer();
        public synchronized void write(Buffer source,long count) throws IOException {
            if (bytes.size()+count>32768) throw new IOException("bounded component output exceeded");
            bytes.write(source,count); notifyAll();
        }
        public Timeout timeout() { return Timeout.NONE; }
        public synchronized void flush() { notifyAll(); }
        public void close() { }
        synchronized long connectionUpdates() throws Exception {
            Buffer frames=bytes.clone();
            if (frames.size()<24) return 0;
            frames.skip(24); // Actual client preface.
            long sum=0;
            while (frames.size()>=9) {
                int length=(frames.readByte()&255)<<16 | (frames.readByte()&255)<<8 | (frames.readByte()&255);
                int type=frames.readByte()&255; frames.readByte(); int stream=frames.readInt()&0x7fffffff;
                if (frames.size()<length) break;
                if (type==8 && stream==0 && length==4) sum+=frames.readInt()&0x7fffffff;
                else frames.skip(length);
            }
            return sum;
        }
    }
    private static final class Harness implements AutoCloseable {
        final ParkedSource input=new ParkedSource();
        final RecordedSink output=new RecordedSink();
        final FramedConnection connection;
        final FrameReader.Handler handler;
        final long initialUpdates;
        Harness() throws Exception {
            Socket boundary=new Socket() { @Override public void close() { input.close(); } };
            connection=new FramedConnection.Builder(true).protocol(Protocol.HTTP_2)
                    .socket(boundary,"bounded-credit-component",Okio.buffer(input),Okio.buffer(output)).build();
            if (!input.entered.await(1,TimeUnit.SECONDS)) throw new AssertionError("reader did not enter");
            connection.sendConnectionPreface();
            initialUpdates=output.connectionUpdates();
            // A public SETTINGS transition after the original preface keeps
            // normal tests below the ack threshold without changing defaults.
            window(1024*1024);
            handler=(FrameReader.Handler)field(connection,"readerRunnable");
        }
        void window(int size) throws Exception {
            Settings settings=new Settings();
            call(settings,"set",new Class<?>[]{int.class,int.class,int.class},7,0,size);
            connection.setSettings(settings);
        }
        FramedStream stream() throws Exception {
            return connection.newStream(Arrays.asList(new Header(":method","GET"),
                    new Header(":scheme","http"),new Header(":authority","component"),
                    new Header(":path","/")),false,true);
        }
        long pending() throws Exception {
            synchronized (connection) { return ((Number)field(connection,"unacknowledgedBytesRead")).longValue(); }
        }
        void accounted(long expected,String name) throws Exception {
            check(pending()==expected,name+" returned exactly "+expected+" bytes (actual "+pending()+")");
        }
        void awaitPending(long expected) throws Exception {
            long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(1);
            while (pending()!=expected && System.nanoTime()<deadline) Thread.sleep(1);
        }
        void padded(FramedStream stream,int payload,int padding,boolean end) throws Exception {
            int length=payload+padding+1;
            Buffer frame=new Buffer().writeByte(length>>>16).writeByte(length>>>8).writeByte(length)
                    .writeByte(0).writeByte(8|(end?1:0)).writeInt(stream.getId()).writeByte(padding);
            frame.write(new byte[payload+padding]);
            try (FrameReader reader=new Http2().newReader(frame,true)) {
                check(reader.nextFrame(handler),"actual HTTP/2 DATA parser accepted bounded frame");
            }
        }
        public void close() throws Exception {
            connection.close(); input.close();
            Thread reader=input.reader;
            if (reader!=null) { reader.join(1000); check(!reader.isAlive(),"provider reader retired"); }
        }
    }

    private static void basicRetirement() throws Exception {
        try (Harness h=new Harness()) {
            FramedStream stream=h.stream(); receive(stream,data(512),512);
            h.accounted(0,"unread bytes remain owned");
            stream.closeLater(ErrorCode.CANCEL); h.accounted(512,"local cancellation");
            stream.closeLater(ErrorCode.CANCEL); stream.getSource().close();
            h.accounted(512,"repeat reset and close do not double return");
            h.handler.data(false,stream.getId(),data(257),257);
            h.accounted(769,"late DATA on removed stream");
            FramedStream survivor=h.stream(); receive(survivor,data(31),31);
            Buffer read=new Buffer(); check(survivor.getSource().read(read,31)==31,"survivor returns body");
            h.accounted(800,"unrelated survivor remains usable");
            survivor.closeLater(ErrorCode.CANCEL); survivor.getSource().close();
            h.accounted(800,"read survivor is not double returned");
        }
        try (Harness h=new Harness()) {
            FramedStream stream=h.stream(); receive(stream,data(512),512);
            Buffer read=new Buffer(); check(stream.getSource().read(read,129)==129,"partial application read");
            h.accounted(129,"partial read only");
            stream.getSource().close(); h.accounted(512,"close returns unread remainder");
            stream.getSource().close(); h.accounted(512,"second source close is idempotent");
        }
        try (Harness h=new Harness()) {
            FramedStream stream=h.stream(); receive(stream,data(512),512);
            h.handler.rstStream(stream.getId(),ErrorCode.CANCEL);
            h.accounted(512,"peer reset discards buffered data");
            h.handler.rstStream(stream.getId(),ErrorCode.CANCEL);
            stream.getSource().close(); h.accounted(512,"peer/local repeated retirement");
        }
        try (Harness h=new Harness()) {
            FramedStream stream=h.stream(); receive(stream,data(512),512); fin(stream);
            stream.closeLater(ErrorCode.CANCEL); h.accounted(0,"both FIN retains unread source for caller");
            stream.getSource().close(); h.accounted(512,"transport's completed-source close returns unread body");
            receive(stream,data(17),17); h.accounted(529,"direct data after completed source");
        }
    }

    private static void paddingAndPush() throws Exception {
        try (Harness h=new Harness()) {
            FramedStream stream=h.stream();
            h.padded(stream,0,255,false); h.accounted(256,"padding-only DATA includes length octet");
            check(((Number)field(stream,"unacknowledgedBytesRead")).longValue()==256,
                    "live stream also returns padding credit");
            h.padded(stream,19,0,false); h.accounted(257,"zero padding still includes length octet");
            Buffer read=new Buffer(); check(stream.getSource().read(read,19)==19,"padding never becomes body");
            h.accounted(276,"body and padding accounted independently");
            h.padded(stream,0,7,true); h.accounted(284,"END_STREAM padding returned to connection");
            stream.getSource().close(); h.accounted(284,"padding not returned twice at close");
        }
        try (Harness h=new Harness()) {
            h.handler.data(true,2,data(512),512); // Default provider PushObserver.CANCEL.
            h.awaitPending(512); h.accounted(512,"rejected push body retired by observer");
        }
    }

    private static void receiveResetRace() throws Exception {
        for (boolean sourceClose:new boolean[]{false,true}) {
            try (Harness h=new Harness()) {
                FramedStream stream=h.stream();
                CountDownLatch entered=new CountDownLatch(1),proceed=new CountDownLatch(1);
                Source boundary=new Source() {
                    final Buffer body=data(512);
                    public long read(Buffer sink,long count) throws IOException {
                        entered.countDown();
                        try { if (!proceed.await(1,TimeUnit.SECONDS)) throw new IOException("race barrier deadline"); }
                        catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException(e); }
                        return body.read(sink,count);
                    }
                    public Timeout timeout() { return Timeout.NONE; }
                    public void close() { }
                };
                AtomicReference<Throwable> failure=new AtomicReference<>();
                Thread worker=new Thread(() -> {
                    try { receive(stream,Okio.buffer(boundary),512); }
                    catch (Throwable t) { failure.set(t); }
                },"bounded-credit-race");
                worker.start();
                try {
                    check(entered.await(1,TimeUnit.SECONDS),"receive paused outside stream monitor");
                    if (sourceClose) stream.getSource().close(); else stream.closeLater(ErrorCode.CANCEL);
                    h.accounted(0,"retirement does not touch reader-owned receive buffer");
                } finally { proceed.countDown(); worker.join(1000); }
                check(!worker.isAlive() && failure.get()==null,"in-flight receive retires without deadlock");
                h.accounted(512,"read completing after retirement returns its bytes");
                stream.getSource().close(); h.accounted(512,"race close remains exactly once");
            }
        }
    }

    private static void thresholdAndOverflow() throws Exception {
        try (Harness h=new Harness()) {
            h.window(1024);
            FramedStream stream=h.stream(); receive(stream,data(512),512);
            stream.closeLater(ErrorCode.CANCEL);
            long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(1);
            while (h.output.connectionUpdates()-h.initialUpdates<512 && System.nanoTime()<deadline) Thread.sleep(1);
            check(h.output.connectionUpdates()-h.initialUpdates==512,"actual writer emits exact connection WINDOW_UPDATE");
            h.accounted(0,"emitted update removes pending connection credit");
        }
        try (Harness h=new Harness()) {
            h.window(1024);
            FramedStream stream=h.stream(); receive(stream,data(31),31);
            receive(stream,data(1025),1025);
            check(stream.getErrorCode()==ErrorCode.FLOW_CONTROL_ERROR,"overflow retains flow-control failure");
            long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(1);
            while (h.output.connectionUpdates()-h.initialUpdates<1025 && System.nanoTime()<deadline) Thread.sleep(1);
            check(h.output.connectionUpdates()-h.initialUpdates+h.pending()==1056,
                    "overflow returns rejected DATA and already-buffered bytes exactly once");
        }
    }

    public static void main(String[] args) throws Exception {
        basicRetirement(); paddingAndPush(); receiveResetRace(); thresholdAndOverflow();
        System.out.println("Http2CreditContracts: "+checks+" checks, "+failures+" failures; in-memory only, max DATA 1025 bytes");
        if (failures!=0) throw new AssertionError("HTTP/2 receive credit ownership failures: "+failures);
    }
}
