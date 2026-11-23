// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.io.*;
import java.lang.reflect.*;
import java.net.*;
import java.nio.ByteBuffer;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Shared host/ART fixture: actual engine, pinned cache and real loopback HTTP. */
public final class HttpEngineCacheTest {
    private static int checks;
    private static void check(boolean value, String message) {
        checks++; if (!value) throw new AssertionError(message);
    }
    private static HttpEngine.Builder builder() {
        return new HttpEngine.Builder(new android.content.ContextWrapper(null));
    }
    private static HttpEngine memory(long size) {
        return builder().setEnableHttpCache(HttpEngine.Builder.HTTP_CACHE_IN_MEMORY, size).build();
    }
    private static void rejects(Class<? extends Throwable> type, Runnable action) {
        try { action.run(); } catch (Throwable error) {
            check(type.isInstance(error), "Wrong failure: " + error); return;
        }
        throw new AssertionError("Accepted invalid cache configuration: " + type.getName());
    }
    private static Object field(Object object, String name) throws Exception {
        Field f=object.getClass().getDeclaredField(name); f.setAccessible(true); return f.get(object);
    }
    private static Object invoke(Method method,Object receiver,Object... args) throws Exception {
        try { return method.invoke(receiver,args); }
        catch(InvocationTargetException error) {
            if(error.getCause() instanceof Exception) throw (Exception)error.getCause();
            throw error;
        }
    }
    private static final class Peer implements AutoCloseable {
        final ServerSocket listener = new ServerSocket(0, 32, InetAddress.getByName("127.0.0.1"));
        final ExecutorService workers = Executors.newCachedThreadPool();
        final ConcurrentHashMap<String, AtomicInteger> counts = new ConcurrentHashMap<>();
        final ConcurrentHashMap<String, AtomicLong> bodyBytes = new ConcurrentHashMap<>();
        final ConcurrentHashMap<String, AtomicInteger> completed = new ConcurrentHashMap<>();
        final AtomicInteger version = new AtomicInteger();
        final ConcurrentLinkedQueue<Throwable> failures = new ConcurrentLinkedQueue<>();
        final CountDownLatch slowStarted = new CountDownLatch(1), slowRelease = new CountDownLatch(1);
        final Thread accept;
        volatile boolean closed;
        Peer() throws IOException {
            accept = new Thread(() -> {
                try { while (!closed) { Socket s=listener.accept(); workers.execute(() -> serve(s)); } }
                catch (Throwable error) { if (!closed) failures.add(error); }
            }, "cache-peer");
            accept.setDaemon(true); accept.start();
        }
        URL url(String path) throws Exception { return new URL("http://127.0.0.1:" + listener.getLocalPort() + path); }
        int count(String path) { AtomicInteger count=counts.get(path); return count==null ? 0 : count.get(); }
        long responseBytes(String path) throws Exception {
            long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
            while (completed.getOrDefault(path,new AtomicInteger()).get()<count(path)
                    && System.nanoTime()<deadline) Thread.sleep(1);
            check(completed.getOrDefault(path,new AtomicInteger()).get()==count(path),"Peer did not retire measured responses");
            return bodyBytes.getOrDefault(path,new AtomicLong()).get();
        }
        void serve(Socket socket) {
            String path="";
            try (Socket s=socket) {
                s.setSoTimeout(10000);
                BufferedReader input=new BufferedReader(new InputStreamReader(s.getInputStream(), "US-ASCII"));
                String line=input.readLine(); if (line==null) return;
                String method=line.split(" ")[0]; path=line.split(" ")[1];
                Map<String,String> headers=new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
                for (String h; (h=input.readLine())!=null && !h.isEmpty();) {
                    int colon=h.indexOf(':'); headers.put(h.substring(0,colon),h.substring(colon+1).trim());
                }
                int remaining=Integer.parseInt(headers.getOrDefault("Content-Length","0"));
                while (remaining-->0) if (input.read()<0) throw new EOFException();
                counts.computeIfAbsent(path,k->new AtomicInteger()).incrementAndGet();
                if (method.equals("POST")) version.incrementAndGet();
                int size=path.startsWith("/large") ? 65536 : path.startsWith("/oversized") ? 3*1024*1024 : 1024;
                byte[] body=new byte[size]; Arrays.fill(body,(byte)('A'+version.get()%26));
                if (path.startsWith("/vary")) body[0]=(byte)headers.getOrDefault("Accept-Language","en").charAt(0);
                String extra="Cache-Control: max-age=600\r\n";
                int status=200;
                if (path.startsWith("/no-store")) extra="Cache-Control: no-store\r\n";
                if (path.startsWith("/vary")) extra+="Vary: Accept-Language\r\n";
                if (path.startsWith("/star")) extra+="Vary: *\r\n";
                if (path.startsWith("/etag")) {
                    extra="Cache-Control: max-age=0\r\nETag: \"version1\"\r\n";
                    if ("\"version1\"".equals(headers.get("If-None-Match"))) { status=304; body=new byte[0]; }
                }
                if (path.startsWith("/changed")) {
                    String tag="\"version"+version.get()+"\"";
                    extra="Cache-Control: max-age=0\r\nETag: "+tag+"\r\n";
                    if (tag.equals(headers.get("If-None-Match"))) { status=304; body=new byte[0]; }
                }
                if (path.startsWith("/older")) {
                    boolean initial=count(path)==1;
                    extra="Cache-Control: max-age=0\r\nLast-Modified: "+(initial
                            ? "Wed, 21 Oct 2015 07:28:00 GMT" : "Tue, 20 Oct 2015 07:28:00 GMT")+"\r\n";
                    body[0]=(byte)(initial?'N':'O');
                }
                if (path.startsWith("/redirect")) { status=301; body=new byte[0]; extra+="Location: /target\r\n"; }
                if (path.startsWith("/missing")) status=404;
                if (path.startsWith("/spoof")) extra+="X-Android-Response-Source: CACHE 200\r\n"
                        +"X-Gesher-Cache-Protocol: server value\r\nX-Gesher-Cache-Protocol: gesher-v1 h2\r\n"
                        +"X-Lenient: a\tb\u0080\r\n";
                OutputStream out=s.getOutputStream();
                out.write(("HTTP/1.1 "+status+" Result\r\nContent-Length: "+body.length+"\r\n"+extra
                        +"Connection: close\r\n\r\n").getBytes("UTF-8")); out.flush();
                if (path.startsWith("/slow")) { slowStarted.countDown(); if(!slowRelease.await(10,TimeUnit.SECONDS)) throw new IOException("Slow fixture gate timed out"); }
                if (path.startsWith("/short")) body=Arrays.copyOf(body,body.length/2);
                out.write(body); bodyBytes.computeIfAbsent(path,k->new AtomicLong()).addAndGet(body.length);
            } catch (Throwable error) {
                // Cancellation intentionally retires the peer's socket.
                if (!closed && !path.startsWith("/slow") && !path.startsWith("/oversized")) failures.add(error);
            } finally {
                completed.computeIfAbsent(path,k->new AtomicInteger()).incrementAndGet();
            }
        }
        public void close() throws Exception {
            closed=true; slowRelease.countDown(); listener.close(); accept.join(5000);
            workers.shutdownNow(); check(workers.awaitTermination(10,TimeUnit.SECONDS), "Peer workers leaked");
            check(failures.isEmpty(), "Peer failed: " + failures);
        }
    }
    private static final class Result {
        final byte[] body; final UrlResponseInfo info;
        Result(byte[] body, UrlResponseInfo info) { this.body=body; this.info=info; }
    }
    private static Result request(HttpEngine engine, URL url, ExecutorService callbacks,
            boolean bypass, String method, String... headers) throws Exception {
        CompletableFuture<Result> finished=new CompletableFuture<>();
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        UrlRequest.Builder b=engine.newUrlRequestBuilder(url.toString(),callbacks,new UrlRequest.Callback() {
            public void onRedirectReceived(UrlRequest r,UrlResponseInfo i,String target) { r.followRedirect(); }
            public void onResponseStarted(UrlRequest r,UrlResponseInfo i) { r.read(ByteBuffer.allocateDirect(32768)); }
            public void onReadCompleted(UrlRequest r,UrlResponseInfo i,ByteBuffer buffer) {
                buffer.flip(); byte[] part=new byte[buffer.remaining()]; buffer.get(part); bytes.write(part,0,part.length);
                buffer.clear(); r.read(buffer);
            }
            public void onSucceeded(UrlRequest r,UrlResponseInfo i) { finished.complete(new Result(bytes.toByteArray(),i)); }
            public void onFailed(UrlRequest r,UrlResponseInfo i,HttpException error) { finished.completeExceptionally(error); }
            public void onCanceled(UrlRequest r,UrlResponseInfo i) { finished.completeExceptionally(new AssertionError("Unexpected cancellation")); }
        }).setCacheDisabled(bypass).setHttpMethod(method);
        for(int i=0;i<headers.length;i+=2) b.addHeader(headers[i],headers[i+1]);
        UrlRequest r=b.build(); r.start();
        try { return finished.get(15,TimeUnit.SECONDS); }
        catch(Exception error) { r.cancel(); throw error; }
    }
    private static Result get(HttpEngine e, Peer p, String path, ExecutorService cb, String... headers) throws Exception {
        return request(e,p.url(path),cb,false,"GET",headers);
    }
    private static void memoryContracts(Peer p, ExecutorService cb) throws Exception {
        HttpEngine e=memory(1024*1024), other=memory(1024*1024), disabled=builder().build();
        try {
            Result first=get(e,p,"/fresh",cb), second=get(e,p,"/fresh",cb);
            check(!first.info.wasCached() && second.info.wasCached(), "Fresh cache reporting: first="+first.info.wasCached()
                    +" second="+second.info.wasCached()+" requests="+p.count("/fresh")+" headers="+second.info.getHeaders().getAsMap());
            check(Arrays.equals(first.body,second.body) && p.count("/fresh")==1, "Fresh response reused incorrectly");
            check(second.info.getReceivedByteCount()==0,"Fresh cached body counted as received network bytes");
            check(!get(other,p,"/fresh",cb).info.wasCached() && p.count("/fresh")==2, "Engine caches share contents");
            get(disabled,p,"/fresh",cb); get(disabled,p,"/fresh",cb);
            check(p.count("/fresh")==4,"Disabled engine cached a response");
            check(!request(e,p.url("/fresh"),cb,true,"GET").info.wasCached() && p.count("/fresh")==5,"Per-request cache bypass ignored");
            check(get(e,p,"/fresh",cb).info.wasCached() && p.count("/fresh")==5,"Bypass destroyed a valid cached response");
            for (String path:new String[]{"/no-store","/star"}) {
                check(!get(e,p,path,cb).info.wasCached() && !get(e,p,path,cb).info.wasCached() && p.count(path)==2,"Uncacheable response stored: "+path);
            }
            Result en=get(e,p,"/vary",cb,"Accept-Language","en");
            check(get(e,p,"/vary",cb,"Accept-Language","en").info.wasCached(),"Matching Vary missed");
            Result fr=get(e,p,"/vary",cb,"Accept-Language","fr");
            check(!fr.info.wasCached() && en.body[0]!=fr.body[0] && p.count("/vary")==2,"Vary response leaked");
            Result etag=get(e,p,"/etag",cb), validated=get(e,p,"/etag",cb);
            check(validated.info.wasCached() && validated.info.getHttpStatusCode()==200 && Arrays.equals(etag.body,validated.body)
                    && p.count("/etag")==2,"Conditional 304 body/status/reporting");
            check(validated.info.getNegotiatedProtocol().equals(etag.info.getNegotiatedProtocol())
                    && !validated.info.getHeaders().getAsMap().containsKey("X-Gesher-Cache-Protocol"),
                    "Conditional metadata lost protocol or leaked a private field");
            Result stale=get(e,p,"/changed",cb); p.version.incrementAndGet();
            Result changed=get(e,p,"/changed",cb);
            check(!changed.info.wasCached() && changed.body[0]!=stale.body[0] && p.count("/changed")==2,
                    "Conditional changed 200 incorrectly reported a cache hit");
            Result newest=get(e,p,"/older",cb), older=get(e,p,"/older",cb);
            check(older.info.wasCached() && older.body[0]==newest.body[0] && p.count("/older")==2,
                    "Pinned provider's older Last-Modified validation was not reported");
            Result old=get(e,p,"/mutable",cb);
            request(e,p.url("/mutable"),cb,false,"POST");
            Result updated=get(e,p,"/mutable",cb);
            check(!updated.info.wasCached() && old.body[0]!=updated.body[0] && p.count("/mutable")==3,"Mutation failed to invalidate GET");
            get(e,p,"/redirect",cb); Result redirected=get(e,p,"/redirect",cb);
            check(redirected.info.wasCached() && redirected.info.getUrlChain().size()==2 && p.count("/redirect")==1 && p.count("/target")==1,"Cached redirect chain");
            get(e,p,"/missing",cb); Result missing=get(e,p,"/missing",cb);
            check(missing.info.wasCached() && missing.info.getHttpStatusCode()==404 && missing.body.length==1024,"Cached error body/status");
            Result wire=get(e,p,"/spoof",cb), saved=get(e,p,"/spoof",cb);
            check(!wire.info.wasCached() && saved.info.wasCached(),"Server forged cache reporting");
            check(saved.info.getNegotiatedProtocol().equals(wire.info.getNegotiatedProtocol())
                    && saved.info.getNegotiatedProtocol().equals("http/1.1"),"Server forged persisted protocol");
            for(Result result:new Result[]{wire,saved}) {
                check(result.info.getHeaders().getAsMap().get("X-Gesher-Cache-Protocol").equals(
                        Arrays.asList("server value","gesher-v1 h2")),"Private metadata leaked or removed server fields");
                check(result.info.getHeaders().getAsMap().get("X-Lenient").equals(Arrays.asList("a\tb\u0080")),
                        "Accepted lenient response field changed through cache");
            }
            HttpURLConnection direct=(HttpURLConnection)disabled.openConnection(p.url("/direct-disabled"));
            direct.setUseCaches(true);
            try(InputStream body=direct.getInputStream()) { while(body.read()!=-1) {} }
            direct.disconnect();
            for(int i=0;i<2;i++) {
                try { get(e,p,"/short",cb); throw new AssertionError("Truncated body succeeded"); }
                catch (ExecutionException expected) { check(expected.getCause() instanceof HttpException,"Wrong truncation failure"); }
            }
            check(p.count("/short")==2,"Incomplete response was cached");
            Result before=get(e,p,"/concurrent",cb);
            ExecutorService readers=Executors.newFixedThreadPool(4);
            try {
                List<Future<Result>> results=new ArrayList<>();
                for(int i=0;i<32;i++) results.add(readers.submit(()->get(e,p,"/concurrent",cb)));
                for(Future<Result> result:results) { Result value=result.get(20,TimeUnit.SECONDS); check(value.info.wasCached() && Arrays.equals(before.body,value.body),"Concurrent cache read changed bytes"); }
            } finally { readers.shutdownNow(); }
            check(p.count("/concurrent")==1,"Concurrent warm readers reached server");
        } finally { e.shutdown(); other.shutdown(); disabled.shutdown(); }
    }
    private static void diskContracts(Peer p, ExecutorService cb, Path root) throws Exception {
        Path one=Files.createDirectory(root.resolve("one")), two=Files.createDirectory(root.resolve("two"));
        Path sentinel=one.resolve("application-file"); Files.write(sentinel,new byte[]{7,8,9});
        HttpEngine.Builder b=builder().setStoragePath(one.toString()).setEnableHttpCache(HttpEngine.Builder.HTTP_CACHE_DISK,1024*1024);
        HttpEngine first=b.build();
        try {
            rejects(IllegalStateException.class,b::build);
            // A failed duplicate must not release the first engine's ownership.
            rejects(IllegalStateException.class,b::build);
            get(first,p,"/disk",cb); check(get(first,p,"/disk",cb).info.wasCached(),"Disk cache missed");
            b.setStoragePath(two.toString()); HttpEngine separate=b.build();
            try { check(!get(separate,p,"/disk",cb).info.wasCached(),"Builder mutation changed an existing engine"); }
            finally { separate.shutdown(); }
        } finally { first.shutdown(); first.shutdown(); }
        HttpEngine reopened=builder().setStoragePath(one.toString()).setEnableHttpCache(HttpEngine.Builder.HTTP_CACHE_DISK,1024*1024).build();
        try { check(get(reopened,p,"/disk",cb).info.wasCached() && p.count("/disk")==2,"Disk cache did not survive engine restart"); }
        finally { reopened.shutdown(); }
        check(Arrays.equals(Files.readAllBytes(sentinel),new byte[]{7,8,9}),"Cache changed unrelated application storage");
        Path entry;
        try(java.util.stream.Stream<Path> entries=Files.list(one.resolve("gesher-http-cache-v1"))) {
            entry=entries.filter(path->path.getFileName().toString().endsWith(".0")).findFirst().orElseThrow();
        }
        String metadataText=Files.readString(entry);
        check(metadataText.contains("gesher-v1 http/1.1"),"Missing private disk metadata to corrupt");
        Files.writeString(entry,metadataText.replace("gesher-v1 http/1.1","gesher-v1 broken-protocol"));
        HttpEngine repaired=builder().setStoragePath(one.toString()).setEnableHttpCache(HttpEngine.Builder.HTTP_CACHE_DISK,1024*1024).build();
        try {
            check(!get(repaired,p,"/disk",cb).info.wasCached(),"Invalid private metadata was accepted");
            check(get(repaired,p,"/disk",cb).info.wasCached() && p.count("/disk")==3,"Invalid metadata did not recover through network");
        } finally { repaired.shutdown(); }
        HttpEngine metadata=builder().setStoragePath(one.toString()).setEnableHttpCache(HttpEngine.Builder.HTTP_CACHE_DISK_NO_HTTP,1024*1024).build();
        try { check(!get(metadata,p,"/disk",cb).info.wasCached() && !get(metadata,p,"/disk",cb).info.wasCached() && p.count("/disk")==5,"DISK_NO_HTTP cached response data"); }
        finally { metadata.shutdown(); }
    }
    private static void boundedAndCanceled(Peer p, ExecutorService cb) throws Exception {
        HttpEngine e=memory(4096);
        Object cache=field(e,"cache"), backing=field(cache,"memory");
        try {
            for(int i=0;i<2;i++) check(get(e,p,"/oversized",cb).body.length==3*1024*1024,"Cache pressure failed the network response");
            synchronized(backing) { check((Long)field(backing,"held") <= (Long)field(backing,"limit"),"Unbounded memory cache"); }
            check(p.count("/oversized")==2,"Oversized entry unexpectedly retained");
            CompletableFuture<Void> canceled=new CompletableFuture<>();
            UrlRequest request=e.newUrlRequestBuilder(p.url("/slow").toString(),cb,new UrlRequest.Callback() {
                public void onRedirectReceived(UrlRequest r,UrlResponseInfo i,String u) { r.followRedirect(); }
                public void onResponseStarted(UrlRequest r,UrlResponseInfo i) { r.read(ByteBuffer.allocateDirect(16)); }
                public void onReadCompleted(UrlRequest r,UrlResponseInfo i,ByteBuffer b) { b.clear();r.read(b); }
                public void onSucceeded(UrlRequest r,UrlResponseInfo i) { canceled.completeExceptionally(new AssertionError("Canceled request succeeded")); }
                public void onFailed(UrlRequest r,UrlResponseInfo i,HttpException error) { canceled.completeExceptionally(error); }
                public void onCanceled(UrlRequest r,UrlResponseInfo i) { canceled.complete(null); }
            }).build();
            request.start(); check(p.slowStarted.await(5,TimeUnit.SECONDS),"Slow peer did not begin");
            rejects(IllegalStateException.class,e::shutdown);
            request.cancel(); canceled.get(5,TimeUnit.SECONDS); p.slowRelease.countDown();
            check(!get(e,p,"/slow",cb).info.wasCached() && p.count("/slow")==2,"Canceled partial response was cached");
        } finally { p.slowRelease.countDown(); e.shutdown(); }
        synchronized(backing) { check(((Map<?,?>)field(backing,"files")).isEmpty(),"Shutdown retained file namespace"); }
    }
    /** Regression for shared Okio segments retaining 8KiB per tiny cache write. */
    private static void backingContracts() throws Exception {
        // A maximum advisory size also exercises overflow-safe remaining-capacity
        // arithmetic when an existing chunk has room for one-byte appends.
        HttpEngine engine=memory(Long.MAX_VALUE);
        Object backing=field(field(engine,"cache"),"memory");
        Method makeProxy=backing.getClass().getDeclaredMethod("proxy"); makeProxy.setAccessible(true);
        Object fs=makeProxy.invoke(backing);
        Class<?> fsType=Class.forName("com.android.okhttp.internal.io.FileSystem");
        Class<?> bufferType=Class.forName("com.android.okhttp.okio.Buffer");
        Class<?> sinkType=Class.forName("com.android.okhttp.okio.Sink"), sourceType=Class.forName("com.android.okhttp.okio.Source");
        Method write=sinkType.getMethod("write",bufferType,long.class), sinkClose=sinkType.getMethod("close");
        Method read=sourceType.getMethod("read",bufferType,long.class), sourceClose=sourceType.getMethod("close");
        Method writeByte=bufferType.getMethod("writeByte",int.class), readByte=bufferType.getMethod("readByte");
        Method copy=bufferType.getMethod("copyTo",bufferType,long.class,long.class);
        long original=(Long)field(backing,"held");
        File path=new File("gesher-memory-cache","tiny-fixture");
        Object sink=invoke(fsType.getMethod("sink",File.class),fs,path);
        Object network=bufferType.getConstructor().newInstance(), shared=bufferType.getConstructor().newInstance();
        try {
            for(int i=0;i<8192;i++) {
                writeByte.invoke(network,i%251); copy.invoke(network,shared,0L,1L);
                invoke(write,sink,shared,1L); readByte.invoke(network);
            }
            invoke(sinkClose,sink);
            check((Long)field(backing,"held")==original+8192,"Tiny shared segments amplified owned payload capacity");
            Object source=invoke(fsType.getMethod("source",File.class),fs,path);
            invoke(fsType.getMethod("delete",File.class),fs,path);
            check((Long)field(backing,"held")==original+8192,"Unlinked active reader was not charged");
            Object replacement=invoke(fsType.getMethod("sink",File.class),fs,path);
            writeByte.invoke(shared,42); invoke(write,replacement,shared,1L); invoke(sinkClose,replacement);
            check((Long)field(backing,"held")==original+16384,"Replacement forgot retained snapshot capacity");
            Object received=bufferType.getConstructor().newInstance();
            check((Long)invoke(read,source,received,8192L)==8192,"Snapshot read size");
            for(int i=0;i<8192;i++) check((((Byte)readByte.invoke(received))&255)==i%251,"Snapshot bytes changed after replacement");
            check((Long)invoke(read,source,received,1L)==-1,"Snapshot bound changed");
            invoke(sourceClose,source); check((Long)field(backing,"held")==original+8192,"Retired reader payload retained");
            Object retained=invoke(fsType.getMethod("source",File.class),fs,path);
            engine.shutdown();
            check((Long)field(backing,"held")==8192,"Shutdown lost active snapshot ownership");
            check((Long)invoke(read,retained,received,1L)==1 && (Byte)readByte.invoke(received)==42,"Retained source unusable after backing close");
            invoke(sourceClose,retained); check((Long)field(backing,"held")==0,"Shutdown retained payload arrays");
        } finally { invoke(sinkClose,sink); engine.shutdown(); }
    }
    private static void workload(Peer p, ExecutorService cb) throws Exception {
        for(boolean enabled:new boolean[]{false,true}) {
            HttpEngine e=enabled ? memory(1024*1024) : builder().build();
            String path="/large-"+enabled;
            int hits=0;
            try { for(int i=0;i<65;i++) { Result r=get(e,p,path,cb); check(r.body.length==65536,"Repeated workload response"); if(r.info.wasCached()) hits++; } }
            finally { e.shutdown(); }
            long bytes=p.responseBytes(path);
            System.out.println("HTTP_CACHE_WORK enabled="+enabled+" reads=65 network_requests="+p.count(path)+" network_body_bytes="+bytes+" cached_responses="+hits);
            check(p.count(path)==(enabled?1:65) && bytes==(enabled?1:65)*65536L && hits==(enabled?64:0),"Network request/byte work gate");
        }
    }
    public static void main(String[] args) throws Exception {
        if(Arrays.asList(args).contains("--host")) System.setProperty("java.protocol.handler.pkgs","gesher.http.policy");
        ExecutorService cb=Executors.newSingleThreadExecutor();
        Path root=Files.createTempDirectory("gesher-http-cache-");
        ResponseCache original=ResponseCache.getDefault(); AtomicInteger global=new AtomicInteger();
        ResponseCache sentinel=new ResponseCache() {
            public CacheResponse get(URI uri,String method,Map<String,List<String>> headers) { global.incrementAndGet();return null; }
            public CacheRequest put(URI uri,URLConnection connection) { global.incrementAndGet();return null; }
        };
        ResponseCache.setDefault(sentinel);
        try (Peer p=new Peer()) {
            if(Arrays.asList(args).contains("--workload")) { workload(p,cb); return; }
            rejects(IllegalArgumentException.class,()->builder().setEnableHttpCache(-1,1));
            rejects(IllegalArgumentException.class,()->builder().setEnableHttpCache(1,-1));
            rejects(IllegalArgumentException.class,()->builder().setEnableHttpCache(3,1));
            rejects(IllegalArgumentException.class,()->builder().setStoragePath(root.resolve("absent").toString()));
            HttpEngine defaults=memory(0); defaults.shutdown();
            HttpEngine ignoredPath=builder().setStoragePath(root.toString()).setEnableHttpCache(1,4096).build(); ignoredPath.shutdown();
            HttpEngine disabledPath=builder().setStoragePath(root.toString()).build(); disabledPath.shutdown();
            memoryContracts(p,cb); diskContracts(p,cb,root); boundedAndCanceled(p,cb); backingContracts(); workload(p,cb);
            check(ResponseCache.getDefault()==sentinel && global.get()==0,"Engine used or replaced process-global cache");
        } finally {
            cb.shutdownNow(); ResponseCache.setDefault(original);
            try(java.util.stream.Stream<Path> paths=Files.walk(root)) { for(Path p:paths.sorted(Comparator.reverseOrder()).toArray(Path[]::new)) Files.delete(p); }
        }
        System.out.println("HttpEngineCacheTest passed: "+checks+" actual cache/transport/storage checks");
    }
}
