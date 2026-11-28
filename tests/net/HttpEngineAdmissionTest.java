// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.io.*;
import java.lang.reflect.*;
import java.net.*;
import java.nio.*;
import java.nio.channels.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.BooleanSupplier;

/** Actual public requests and provider sockets. The peer uses one selector,
 * so server threads cannot be mistaken for a client-side thread reduction. */
public final class HttpEngineAdmissionTest {
    private static final AtomicInteger checks = new AtomicInteger();
    private static final byte[] BODY = new byte[4096];
    static { for (int i=0;i<BODY.length;i++) BODY[i]=(byte)(i*31+17); }
    private static void check(boolean value,String message) {
        checks.incrementAndGet(); if(!value)throw new AssertionError(message);
    }
    private static void until(BooleanSupplier predicate,String message) throws Exception {
        long limit=System.nanoTime()+TimeUnit.SECONDS.toNanos(15);
        while(!predicate.getAsBoolean() && System.nanoTime()<limit)Thread.sleep(2);
        check(predicate.getAsBoolean(),message);
    }
    private static void await(CountDownLatch latch,String message) throws Exception {
        check(latch.await(15,TimeUnit.SECONDS),message);
    }
    private static void gate(CountDownLatch latch) {
        boolean interrupted=false;
        try { for(;;)try { if(!latch.await(30,TimeUnit.SECONDS))throw new AssertionError("Gate timeout"); return; }
            catch(InterruptedException expected){interrupted=true;} }
        finally {if(interrupted)Thread.currentThread().interrupt();}
    }
    private static HttpEngine engine() {
        return new HttpEngine.Builder(new android.content.ContextWrapper(null)).build();
    }
    private static Object field(Object target,String name) throws Exception {
        Field f=target.getClass().getDeclaredField(name);f.setAccessible(true);return f.get(target);
    }
    private static int scheduler(String name) {
        try {
            Class<?> type=Class.forName("android.net.http.HttpRequestScheduler");
            Field lock=type.getDeclaredField("LOCK");lock.setAccessible(true);
            synchronized(lock.get(null)) {
                Field value=type.getDeclaredField(name);value.setAccessible(true);
                Object result=value.get(null);return result instanceof Number?((Number)result).intValue():((Map<?,?>)result).size();
            }
        }catch(Exception error){throw new AssertionError(error);}
    }
    private static void retired() throws Exception {
        until(()->scheduler("outstanding")==0,"Admission owners did not retire");
        check(scheduler("active")==0 && scheduler("metadata")==0 && scheduler("GROUPS")==0,
                "Retired request retained scheduler resources");
    }
    private static int clientThreads() {
        int count=0;for(Thread t:Thread.getAllStackTraces().keySet())
            if(t.isAlive() && t.getName().startsWith("Gesher-HttpEngine"))count++;
        return count;
    }

    private static final class Peer implements AutoCloseable {
        final ServerSocketChannel listener=ServerSocketChannel.open();
        final Selector selector=Selector.open();
        final AtomicInteger requests=new AtomicInteger(),open=new AtomicInteger();
        final ConcurrentLinkedQueue<String> order=new ConcurrentLinkedQueue<>();
        final Set<String> allowed=ConcurrentHashMap.newKeySet();
        final AtomicReference<Throwable> failure=new AtomicReference<>();
        final Thread thread;
        volatile boolean all,closed;
        final class Client {
            final SocketChannel socket;
            final ByteBuffer input=ByteBuffer.allocate(16384);
            ByteBuffer output;
            String path;
            Client(SocketChannel socket){this.socket=socket;}
            void close() throws IOException {if(socket.isOpen()){socket.close();open.decrementAndGet();}}
            void response() {
                byte[] header=("HTTP/1.1 200 OK\r\nContent-Length: "+BODY.length+
                        "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII);
                output=ByteBuffer.allocate(header.length+BODY.length);output.put(header).put(BODY).flip();
                socket.keyFor(selector).interestOps(SelectionKey.OP_WRITE);
            }
        }
        Peer() throws Exception {
            listener.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"),0),2048);
            listener.configureBlocking(false);listener.register(selector,SelectionKey.OP_ACCEPT);
            thread=new Thread(this::run,"admission-peer-selector");thread.setDaemon(true);thread.start();
        }
        String url(String path)throws IOException{return "http://127.0.0.1:"+((InetSocketAddress)listener.getLocalAddress()).getPort()+path;}
        void allow(String path){allowed.add(path);selector.wakeup();}
        void release(){all=true;selector.wakeup();}
        void run() {
            try {
                while(!closed) {
                    for(SelectionKey key:selector.keys())if(key.isValid() && key.attachment()!=null) {
                        Client c=(Client)key.attachment();
                        if(c.path!=null && c.output==null && (all||allowed.contains(c.path)))c.response();
                    }
                    selector.select(5000);
                    Iterator<SelectionKey> selected=selector.selectedKeys().iterator();
                    while(selected.hasNext()) {
                        SelectionKey key=selected.next();selected.remove();if(!key.isValid())continue;
                        if(key.isAcceptable()) {
                            for(SocketChannel socket;(socket=listener.accept())!=null;) {
                                socket.configureBlocking(false);open.incrementAndGet();
                                socket.register(selector,SelectionKey.OP_READ,new Client(socket));
                            }
                            continue;
                        }
                        Client c=(Client)key.attachment();
                        try {
                            if(key.isReadable()) {
                                int n=c.socket.read(c.input);
                                if(n<0){c.close();continue;}
                                if(c.path==null) {
                                    String text=new String(c.input.array(),0,c.input.position(),StandardCharsets.US_ASCII);
                                    if(text.contains("\r\n\r\n")) {
                                        c.path=text.substring(0,text.indexOf("\r\n")).split(" ")[1];
                                        order.add(c.path);requests.incrementAndGet();
                                    } else if(!c.input.hasRemaining())throw new IOException("Peer header bound");
                                }
                            }
                            if(key.isValid() && key.isWritable()) {
                                c.socket.write(c.output);if(!c.output.hasRemaining())c.close();
                            }
                        }catch(SocketException canceled){c.close();}
                    }
                }
            }catch(Throwable error){if(!closed)failure.set(error);}
            finally {
                try {for(SelectionKey key:selector.keys())key.channel().close();selector.close();}
                catch(IOException ignored){}
            }
        }
        void healthy(){if(failure.get()!=null)throw new AssertionError("Peer failed",failure.get());}
        public void close()throws Exception{closed=true;selector.wakeup();thread.join(5000);check(!thread.isAlive(),"Peer leaked");healthy();}
    }

    private interface ResponseAction {void run(UrlRequest request)throws Exception;}
    private static final class Call implements UrlRequest.Callback {
        final CompletableFuture<Call> done=new CompletableFuture<>();
        final AtomicInteger terminals=new AtomicInteger();
        final ByteBuffer buffer=ByteBuffer.allocateDirect(8192);
        UrlRequest request;
        volatile boolean canceled;
        volatile HttpException failure;
        int bytes;
        ResponseAction beforeRead;
        Runnable terminalAction;
        Call(HttpEngine engine,Peer peer,String path,Executor executor,int priority,boolean direct)throws Exception {
            this(engine,peer,path,executor,priority,direct,builder->{});
        }
        Call(HttpEngine engine,Peer peer,String path,Executor executor,int priority,boolean direct,
                java.util.function.Consumer<UrlRequest.Builder> configure)throws Exception {
            UrlRequest.Builder builder=engine.newUrlRequestBuilder(peer.url(path),executor,this)
                    .setPriority(priority).setDirectExecutorAllowed(direct);
            configure.accept(builder);request=builder.build();
        }
        void start(){request.start();}
        public void onRedirectReceived(UrlRequest request,UrlResponseInfo info,String location){request.followRedirect();}
        public void onResponseStarted(UrlRequest request,UrlResponseInfo info) {
            try {check(info.getHttpStatusCode()==200,"Response changed");if(beforeRead!=null)beforeRead.run(request);request.read(buffer);}
            catch(Throwable error){done.completeExceptionally(error);request.cancel();}
        }
        public void onReadCompleted(UrlRequest request,UrlResponseInfo info,ByteBuffer buffer) {
            try {
                buffer.flip();while(buffer.hasRemaining()) {
                    byte value=buffer.get();
                    if(bytes>=BODY.length || value!=BODY[bytes])throw new AssertionError("Response bytes changed at "+bytes);
                    bytes++;
                }
                check(true,"All response bytes compared");
                buffer.clear();request.read(buffer);
            }catch(Throwable error){done.completeExceptionally(error);request.cancel();}
        }
        private void complete(boolean success) {
            try {
                check(terminals.incrementAndGet()==1,"Duplicate terminal callback");
                if(success)check(bytes==BODY.length,"Incomplete successful response");
                if(terminalAction!=null)terminalAction.run();done.complete(this);
            }catch(Throwable error){done.completeExceptionally(error);}
        }
        public void onSucceeded(UrlRequest request,UrlResponseInfo info){complete(true);}
        public void onCanceled(UrlRequest request,UrlResponseInfo info){canceled=true;complete(false);}
        public void onFailed(UrlRequest request,UrlResponseInfo info,HttpException error){failure=error;complete(false);}
        Call await()throws Exception{return done.get(40,TimeUnit.SECONDS);}
    }
    private static Call call(HttpEngine e,Peer p,String path,Executor executor,List<Call> calls)throws Exception {
        Call c=new Call(e,p,path,executor,UrlRequest.REQUEST_PRIORITY_MEDIUM,false);calls.add(c);c.start();return c;
    }
    private static void finish(List<Call> calls,boolean success)throws Exception {
        for(Call c:calls){c.await();if(success)check(!c.canceled && c.failure==null,"Valid request failed: "+c.failure);}
    }
    private static void cancel(List<Call> calls){for(Call c:calls)c.request.cancel();}

    private static Long processCpu() {
        try {
            Object bean=Class.forName("java.lang.management.ManagementFactory").getMethod("getOperatingSystemMXBean").invoke(null);
            return (Long)Class.forName("com.sun.management.OperatingSystemMXBean").getMethod("getProcessCpuTime").invoke(bean);
        }catch(ReflectiveOperationException unavailable){return null;}
    }
    private static Long residentBytes() {
        try(BufferedReader lines=new BufferedReader(new FileReader("/proc/self/status"))) {
            for(String line;(line=lines.readLine())!=null;)if(line.startsWith("VmRSS:"))
                return Long.parseLong(line.trim().split("\\s+")[1])*1024;
        }catch(IOException|NumberFormatException unavailable){}
        return null;
    }

    private static void burst(int count,boolean baseline)throws Exception {
        HttpEngine engine=engine();ExecutorService callbacks=Executors.newFixedThreadPool(4);List<Call> calls=new ArrayList<>();
        try(Peer peer=new Peer()) {
            try {
                Long cpuWorkBefore=processCpu();long wholeStart=System.nanoTime();
                for(int i=0;i<count;i++)call(engine,peer,"/burst/"+i,callbacks,calls);
                int expected=baseline?count:Math.min(32,count);
                until(()->peer.requests.get()>=expected,"Burst never admitted its allowed backend work");
                Thread.sleep(200);peer.healthy();
                int sockets=peer.requests.get(),threads=clientThreads();
                check(sockets==expected,"Burst socket budget: "+sockets+" expected "+expected);
                if(!baseline){
                    check(scheduler("outstanding")==count,"Accepted backlog lost");
                    // Earlier bursts can leave parked workers until the normal
                    // idle timeout; 128 I/O + 4 cleanup + 4 completion is the
                    // process ceiling, independent of current engine activity.
                    check(threads<=136,"Burst process worker budget "+threads);
                }
                Long resident=residentBytes(),cpuBefore=processCpu();
                long start=System.nanoTime();peer.release();finish(calls,true);long elapsed=System.nanoTime()-start;
                Long cpuAfter=processCpu();
                long wholeElapsed=System.nanoTime()-wholeStart;
                check(peer.requests.get()==count,"Accepted request never reached real peer");
                if(!baseline)retired();
                System.out.println("HTTP_ADMISSION_WORK {\"requests\":"+count+",\"baseline\":"+baseline+
                        ",\"blocked_connections\":"+sockets+",\"client_threads\":"+threads+
                        ",\"decoded_bytes\":"+(count*(long)BODY.length)+",\"drain_elapsed_ns\":"+elapsed+
                        ",\"whole_elapsed_ns_including_fixed_hold\":"+wholeElapsed+
                        ",\"whole_process_cpu_ns\":"+(cpuAfter==null||cpuWorkBefore==null?null:cpuAfter-cpuWorkBefore)+
                        ",\"drain_process_cpu_ns\":"+(cpuAfter==null||cpuBefore==null?null:cpuAfter-cpuBefore)+
                        ",\"blocked_process_rss_bytes\":"+resident+"}");
            }finally{cancel(calls);peer.release();finish(calls,false);engine.shutdown();callbacks.shutdownNow();}
        }
    }

    private static void cancellationAndCapacity()throws Exception {
        HttpEngine engine=engine();ExecutorService callbacks=Executors.newFixedThreadPool(4);List<Call> calls=new ArrayList<>();
        try(Peer peer=new Peer()) {
            try {
                for(int i=0;i<1024;i++)call(engine,peer,"/capacity/"+i,callbacks,calls);
                until(()->peer.requests.get()==32,"Initial capacity did not start");
                CompletableFuture<Integer> status=new CompletableFuture<>();calls.get(100).request.getStatus(status::complete);
                check(status.get(5,TimeUnit.SECONDS)==UrlRequest.Status.WAITING_FOR_AVAILABLE_SOCKET,"Queued status");
                Call overflow=call(engine,peer,"/overflow",callbacks,calls);overflow.await();
                check(overflow.failure instanceof NetworkException && !overflow.canceled,"Overflow fabricated success");
                check(((NetworkException)overflow.failure).getErrorCode()==NetworkException.ERROR_OTHER,"Overflow error contract");
                check(scheduler("outstanding")==1024,"Overflow retained a ticket");
                boolean shutdownBlocked=false;try{engine.shutdown();}catch(IllegalStateException expected){shutdownBlocked=true;}
                check(shutdownBlocked,"Queued owners were omitted from engine shutdown");
                for(int i=32;i<128;i++)calls.get(i).request.cancel();
                for(int i=32;i<128;i++)check(calls.get(i).await().canceled,"Queued cancellation not delivered");
                check(peer.requests.get()==32,"Canceled queued requests performed I/O");
                peer.allow("/capacity/0");calls.get(0).await();
                until(()->peer.order.contains("/capacity/128"),"Canceled queue holes stranded its next owner");
                for(int i=32;i<128;i++)check(!peer.order.contains("/capacity/"+i),"Canceled request reached peer");
                cancel(calls);finish(calls,false);retired();
                until(()->peer.open.get()==0,"Canceled socket remained open");
                check(!peer.order.contains("/overflow"),"Overflow performed I/O");
            }finally{cancel(calls);peer.release();finish(calls,false);engine.shutdown();callbacks.shutdownNow();}
        }
    }

    private static void fairnessAndPriority()throws Exception {
        List<HttpEngine> engines=new ArrayList<>();List<Call> calls=new ArrayList<>();
        ExecutorService callbacks=Executors.newFixedThreadPool(4);
        try(Peer peer=new Peer()) {
            try {
                for(int e=0;e<5;e++)engines.add(engine());
                for(int e=0;e<4;e++)for(int i=0;i<64;i++)call(engines.get(e),peer,"/engine/"+e+"/"+i,callbacks,calls);
                until(()->peer.requests.get()==128,"Global backend capacity not filled");
                Call low=new Call(engines.get(4),peer,"/low",callbacks,UrlRequest.REQUEST_PRIORITY_IDLE,false);
                calls.add(low);low.start();
                Call high=new Call(engines.get(4),peer,"/high",callbacks,UrlRequest.REQUEST_PRIORITY_HIGHEST,false);
                calls.add(high);high.start();
                Thread.sleep(100);check(peer.requests.get()==128,"Global admission exceeded128");
                peer.allow("/engine/0/0");calls.get(0).await();
                until(()->peer.order.contains("/high"),"New engine/priority could not progress");
                check(!peer.order.contains("/low") && !peer.order.contains("/engine/0/32"),"Admission fairness/priority changed");
                peer.release();finish(calls,true);retired();
                System.out.println("HTTP_ADMISSION_FAIRNESS engines=5 global_active=128 high_before_idle=true all_completed=258");
            }finally{cancel(calls);peer.release();finish(calls,false);for(HttpEngine e:engines)e.shutdown();callbacks.shutdownNow();}
        }
    }

    private static final class HeldRead extends FilterInputStream {
        final CountDownLatch entered,release;
        HeldRead(InputStream source,CountDownLatch entered,CountDownLatch release){super(source);this.entered=entered;this.release=release;}
        public int read(byte[] bytes,int offset,int count)throws IOException {
            int n=in.read(bytes,offset,count);entered.countDown();gate(release);return n;
        }
    }
    private static void uninterruptibleRetirement()throws Exception {
        HttpEngine engine=engine();ExecutorService callbacks=Executors.newFixedThreadPool(4);List<Call> calls=new ArrayList<>();
        CountDownLatch entered=new CountDownLatch(32),release=new CountDownLatch(1);
        try(Peer peer=new Peer()) {
            try {
                peer.release();
                for(int i=0;i<32;i++) {
                    Call c=new Call(engine,peer,"/held/"+i,callbacks,UrlRequest.REQUEST_PRIORITY_MEDIUM,false);
                    c.beforeRead=request->{
                        Field f=request.getClass().getDeclaredField("input");f.setAccessible(true);
                        f.set(request,new HeldRead((InputStream)f.get(request),entered,release));
                    };
                    calls.add(c);c.start();
                }
                await(entered,"Real provider reads did not enter their external return boundary");
                for(Call c:calls)c.request.cancel();
                Call queued=call(engine,peer,"/after-held",callbacks,calls);
                Thread.sleep(200);
                check(scheduler("active")==32 && !peer.order.contains("/after-held"),"Cancel released an in-flight backend slot");
                for(int i=0;i<32;i++)check(!calls.get(i).done.isDone(),"Unretired worker published terminal callback");
                release.countDown();finish(calls,false);retired();
                check(!queued.canceled && queued.failure==null,"Queued work failed after actual retirement");
            }finally{release.countDown();cancel(calls);peer.release();finish(calls,false);engine.shutdown();callbacks.shutdownNow();}
        }
    }

    private static void directCallbacksCannotBlockCancellation()throws Exception {
        HttpEngine engine=engine();ExecutorService callbacks=Executors.newFixedThreadPool(4);List<Call> calls=new ArrayList<>();
        CountDownLatch entered=new CountDownLatch(4),release=new CountDownLatch(1);
        try(Peer peer=new Peer()) {
            try {
                for(int i=0;i<4;i++) {
                    String path="/direct/"+i;peer.allow(path);
                    Call c=new Call(engine,peer,path,Runnable::run,UrlRequest.REQUEST_PRIORITY_MEDIUM,true);
                    c.terminalAction=()->{entered.countDown();gate(release);};calls.add(c);c.start();
                }
                await(entered,"Direct terminal callbacks did not occupy the handoff workers");
                for(int i=0;i<32;i++)call(engine,peer,"/cancel/"+i,callbacks,calls);
                until(()->peer.requests.get()==36,"Cancellation probe never reached the backend");
                for(int i=4;i<calls.size();i++)calls.get(i).request.cancel();
                until(()->scheduler("active")==0,"Application callbacks blocked transport retirement");
                until(()->peer.open.get()==0,"Application callbacks blocked socket cancellation");
                check(!calls.get(4).done.isDone(),"Blocked callback executor unexpectedly ran its pending handoff");
                release.countDown();finish(calls,false);retired();
                for(int i=4;i<calls.size();i++)check(calls.get(i).canceled,"Cancellation changed terminal result");
            }finally{release.countDown();cancel(calls);peer.release();finish(calls,false);engine.shutdown();callbacks.shutdownNow();}
        }
    }

    private static Object schedulerObject(String name)throws Exception {
        Field f=Class.forName("android.net.http.HttpRequestScheduler").getDeclaredField(name);
        f.setAccessible(true);return f.get(null);
    }
    private static int workerCount(Object pool,String name) {
        try {synchronized(pool){return (Integer)field(pool,name);}}
        catch(Exception error){throw new AssertionError(error);}
    }
    private static Executor workers(int limit,int capacity)throws Exception {
        Class<?> type=Class.forName("android.net.http.HttpRequestScheduler$Workers");
        Constructor<?> ctor=type.getDeclaredConstructor(String.class,int.class,int.class,long.class);
        ctor.setAccessible(true);
        return (Executor)ctor.newInstance("admission-worker-contract",limit,capacity,TimeUnit.MILLISECONDS.toNanos(60));
    }
    private static void boundedWorkers()throws Exception {
        Executor pool=workers(8,8);CountDownLatch warm=new CountDownLatch(1);
        pool.execute(warm::countDown);await(warm,"Worker warmup");
        until(()->workerCount(pool,"idle")==1,"Worker did not park");
        CountDownLatch entered=new CountDownLatch(8),release=new CountDownLatch(1);
        try {
            // The notified idle worker cannot consume the first task while
            // this lock is held. Later submissions must still grow capacity.
            synchronized(pool) {
                for(int i=0;i<8;i++)pool.execute(()->{entered.countDown();gate(release);});
            }
            await(entered,"An idle worker notification stranded independent queued work");
            check(workerCount(pool,"workers")==8,"Demand growth exceeded its worker bound");
        }finally{release.countDown();}
        until(()->workerCount(pool,"workers")==0,"Idle worker capacity never retired");
        for(int round=0;round<20;round++) {
            CountDownLatch done=new CountDownLatch(1);pool.execute(done::countDown);await(done,"Retired pool stranded new work");
            until(()->workerCount(pool,"workers")==0,"Reused pool did not retire");
        }
        Executor limited=workers(2,2);CountDownLatch active=new CountDownLatch(2),unblock=new CountDownLatch(1),finished=new CountDownLatch(4);
        try {
            for(int i=0;i<2;i++)limited.execute(()->{active.countDown();gate(unblock);finished.countDown();});
            await(active,"Bounded workers did not start");
            for(int i=0;i<2;i++)limited.execute(finished::countDown);
            boolean rejected=false;try{limited.execute(()->{throw new AssertionError("Ran rejected work inline");});}
            catch(RejectedExecutionException expected){rejected=true;}
            check(rejected && workerCount(limited,"workers")==2,"Worker/queue bound was not enforced");
        }finally{unblock.countDown();}
        await(finished,"Bounded worker queue lost accepted work");
        until(()->workerCount(limited,"workers")==0,"Bounded pool retained idle threads");
    }

    private static void cancellationBeforeAdmission()throws Exception {
        HttpEngine engine=engine();ExecutorService callbacks=Executors.newFixedThreadPool(2);
        try(Peer peer=new Peer()) {
            Call call=new Call(engine,peer,"/canceled-before-admission",callbacks,UrlRequest.REQUEST_PRIORITY_MEDIUM,false);
            AtomicReference<Throwable> error=new AtomicReference<>();
            Thread starter=new Thread(()->{try{call.start();}catch(Throwable failure){error.set(failure);}},"admission-start-race");
            try {
                synchronized(schedulerObject("LOCK")) {
                    starter.start();
                    until(()->{try{return (Integer)field(call.request,"phase")==1;}catch(Exception failure){throw new AssertionError(failure);}},
                            "Request did not register before admission");
                    call.request.cancel();
                }
                starter.join(5000);check(!starter.isAlive() && error.get()==null,"Canceled start failed: "+error.get());
                check(call.await().canceled,"Cancel before submission lost its terminal callback");
                retired();check(peer.requests.get()==0,"Canceled-before-submit request performed I/O");
            }finally{call.request.cancel();peer.release();starter.join(5000);engine.shutdown();callbacks.shutdownNow();}
        }
    }

    private static void processCapacity()throws Exception {
        List<HttpEngine> engines=new ArrayList<>();List<Call> calls=new ArrayList<>();
        ExecutorService callbacks=Executors.newFixedThreadPool(4);
        try(Peer peer=new Peer()) {
            try {
                for(int e=0;e<5;e++)engines.add(engine());
                for(int e=0;e<4;e++)for(int i=0;i<1024;i++)call(engines.get(e),peer,"/process/"+e+"/"+i,callbacks,calls);
                until(()->peer.requests.get()==128,"Process capacity did not admit its active bound");
                Call overflow=call(engines.get(4),peer,"/process-overflow",callbacks,calls);overflow.await();
                check(overflow.failure instanceof NetworkException && !overflow.canceled,"Process quota fabricated success");
                check(scheduler("outstanding")==4096 && scheduler("active")==128,"Process outstanding/active budget");
                check(!peer.order.contains("/process-overflow"),"Process quota performed I/O");
                cancel(calls);finish(calls,false);retired();
            }finally{cancel(calls);peer.release();finish(calls,false);for(HttpEngine e:engines)e.shutdown();callbacks.shutdownNow();}
        }
    }

    private static final class UploadProbe extends UploadDataProvider {
        final AtomicInteger operations=new AtomicInteger(),closed=new AtomicInteger();
        public long getLength(){operations.incrementAndGet();return 1;}
        public void read(UploadDataSink sink,ByteBuffer buffer){operations.incrementAndGet();buffer.put((byte)1);sink.onReadSucceeded(false);}
        public void rewind(UploadDataSink sink){operations.incrementAndGet();sink.onRewindSucceeded();}
        public void close(){closed.incrementAndGet();}
    }
    private static void metadataAndFailureDelivery()throws Exception {
        HttpEngine engine=engine();ExecutorService callbacks=Executors.newFixedThreadPool(4);List<Call> calls=new ArrayList<>();
        String large="x".repeat(3<<20),medium="y".repeat(2<<20),tooLarge="z".repeat(4<<20);
        try(Peer peer=new Peer()) {
            try {
                for(int i=0;i<32;i++)call(engine,peer,"/metadata-active/"+i,callbacks,calls);
                until(()->peer.requests.get()==32,"Metadata probe did not fill active capacity");
                UploadProbe queuedUpload=new UploadProbe(),overflowUpload=new UploadProbe();
                Call queued=new Call(engine,peer,"/large-queued",callbacks,UrlRequest.REQUEST_PRIORITY_MEDIUM,false,
                        b->b.addHeader("X-Large",large).addHeader("Content-Type","application/octet-stream").setUploadDataProvider(queuedUpload,callbacks));
                calls.add(queued);queued.start();
                Call overflow=new Call(engine,peer,"/metadata-overflow",callbacks,UrlRequest.REQUEST_PRIORITY_MEDIUM,false,
                        b->b.addHeader("X-Medium",medium).addHeader("Content-Type","application/octet-stream").setUploadDataProvider(overflowUpload,callbacks));
                calls.add(overflow);overflow.start();overflow.await();
                check(overflow.failure instanceof NetworkException,"Retained metadata quota did not reject");
                check(scheduler("outstanding")==33 && scheduler("metadata")>(6<<20),"Metadata charge lost its queued owner");
                check(queuedUpload.operations.get()==0 && overflowUpload.operations.get()==0,"Queued/rejected upload ran provider work");
                queued.request.cancel();check(queued.await().canceled,"Queued upload cancellation");
                until(()->queuedUpload.closed.get()==1 && overflowUpload.closed.get()==1,"Upload owners did not close exactly once");
                check(queuedUpload.operations.get()==0,"Canceled queued upload started");
                cancel(calls);finish(calls,false);retired();

                AtomicInteger reentries=new AtomicInteger();AtomicReference<Call> next=new AtomicReference<>();
                Call reentrant=new Call(engine,peer,"/direct-overflow",Runnable::run,UrlRequest.REQUEST_PRIORITY_MEDIUM,true,
                        b->b.addHeader("X-Too-Large",tooLarge));
                reentrant.terminalAction=()->{try {
                    check(reentrant.failure instanceof NetworkException,"Direct rejection lost failure");
                    Call c=call(engine,peer,"/after-reentrant-failure",callbacks,calls);next.set(c);reentries.incrementAndGet();
                }catch(Exception failure){throw new RuntimeException(failure);}};
                calls.add(reentrant);reentrant.start();reentrant.await();
                check(reentries.get()==1 && reentrant.terminals.get()==1,"Failure callback reentry/terminal contract");
                peer.allow("/after-reentrant-failure");check(next.get().await().failure==null,"Failure reentry stranded request");retired();

                AtomicInteger attempts=new AtomicInteger();
                Executor reject=task->{attempts.incrementAndGet();throw new RejectedExecutionException("application executor closed");};
                Call rejected=new Call(engine,peer,"/executor-rejected",reject,UrlRequest.REQUEST_PRIORITY_MEDIUM,false,
                        b->b.addHeader("X-Too-Large",tooLarge));
                rejected.start();retired();
                check(attempts.get()==1 && rejected.terminals.get()==0 && rejected.request.isDone(),"Rejected executor retained ownership or fabricated callback");
                Call throwing=new Call(engine,peer,"/callback-throws",Runnable::run,UrlRequest.REQUEST_PRIORITY_MEDIUM,true,
                        b->b.addHeader("X-Too-Large",tooLarge));
                throwing.terminalAction=()->{throw new IllegalStateException("application callback failed");};throwing.start();
                boolean callbackFailed=false;try{throwing.await();}catch(ExecutionException expected){callbackFailed=true;}
                check(callbackFailed && throwing.terminals.get()==1,"Throwing callback got a duplicate terminal");retired();
                check(!peer.order.contains("/large-queued") && !peer.order.contains("/metadata-overflow")
                        && !peer.order.contains("/direct-overflow") && !peer.order.contains("/executor-rejected")
                        && !peer.order.contains("/callback-throws"),"Rejected/canceled metadata reached the transport");
            }finally{cancel(calls);peer.release();finish(calls,false);engine.shutdown();callbacks.shutdownNow();}
        }
    }

    private static void delayedExecutorRejection(boolean terminal,boolean queuedStatus)throws Exception {
        HttpEngine engine=engine();CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1),rejected=new CountDownLatch(1);
        AtomicInteger attempts=new AtomicInteger();AtomicReference<Throwable> unexpected=new AtomicReference<>();
        Executor executor=task->{
            if(attempts.incrementAndGet()>1){rejected.countDown();throw new RejectedExecutionException("closed after first callback");}
            Thread thread=new Thread(task,"admission-application-callback");
            thread.setUncaughtExceptionHandler((t,error)->{if(!(error instanceof RejectedExecutionException))unexpected.set(error);});
            thread.start();
        };
        try(Peer peer=new Peer()) {
            Call call=new Call(engine,peer,"/delayed-rejection",executor,UrlRequest.REQUEST_PRIORITY_MEDIUM,false);
            try {
                call.request.getStatus(status->{entered.countDown();gate(release);});
                await(entered,"Initial status callback did not start");
                if(queuedStatus)call.request.getStatus(status->{throw new AssertionError("Rejected status callback ran");});
                if(!terminal)peer.release();
                call.start();if(terminal)call.request.cancel();
                Object serial=field(call.request,"callbacks");
                until(()->{try{synchronized(serial){return ((ArrayDeque<?>)field(serial,"tasks")).size()>=(queuedStatus?2:1);}}
                    catch(Exception error){throw new AssertionError(error);}},"Terminal/response did not queue behind application callback");
                if(!terminal)check(!call.request.isDone(),"Response rejection probe already terminal");
                release.countDown();await(rejected,"Delayed executor rejection not reached");
                until(call.request::isDone,"Dropped nonterminal callback retained an active request");
                retired();check(call.terminals.get()==0,"Rejected executor fabricated a terminal callback");
                check(unexpected.get()==null,"Unexpected application worker failure: "+unexpected.get());
            }finally{release.countDown();call.request.cancel();peer.release();engine.shutdown();}
        }
    }

    public static void main(String[] args)throws Exception {
        List<String> options=Arrays.asList(args);
        if(options.contains("--host"))System.setProperty("java.protocol.handler.pkgs","gesher.http.policy");
        int burst=options.indexOf("--burst");
        if(burst>=0){burst(Integer.parseInt(args[burst+1]),options.contains("--baseline"));return;}
        boundedWorkers();cancellationBeforeAdmission();
        for(int count:new int[]{1,32,256,1024})burst(count,false);
        cancellationAndCapacity();processCapacity();
        fairnessAndPriority();uninterruptibleRetirement();directCallbacksCannotBlockCancellation();metadataAndFailureDelivery();
        for(boolean terminal:new boolean[]{false,true})for(boolean queuedStatus:new boolean[]{false,true})
            delayedExecutorRejection(terminal,queuedStatus);
        System.out.println("HttpEngineAdmissionTest passed: "+checks.get()+" real-provider admission/lifetime checks");
    }
}
