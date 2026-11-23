// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import java.io.*;
import java.net.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.zip.GZIPOutputStream;

/** Same actual native provider/engine fixture on a checked host JVM and ART. */
public final class HttpEngineBrotliTest {
    private static int checks;
    private static final byte[] PAYLOAD = payload();
    private static void check(boolean value, String message) {
        checks++; if (!value) throw new AssertionError(message);
    }
    private interface Checked { void run() throws Exception; }
    private static void rejects(Class<? extends Throwable> type, Checked action) throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            check(type.isInstance(failure), "Wrong failure: "+failure); return;
        }
        throw new AssertionError("Expected "+type.getName());
    }
    private static byte[] payload() {
        StringBuilder value = new StringBuilder();
        for (int i=0;i<8192;i++) value.append("{\"id\":").append(i)
                .append(",\"title\":\"Android application record ").append(i%73)
                .append("\",\"enabled\":true,\"description\":\"A real incremental HTTP response keeps callbacks and cancellation responsive.\"}\n");
        return value.toString().getBytes(StandardCharsets.UTF_8);
    }
    private static byte[] gzip(byte[] data) throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        try (GZIPOutputStream out=new GZIPOutputStream(bytes)) { out.write(data); }
        return bytes.toByteArray();
    }
    private static byte[] read(InputStream in,int chunk) throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        byte[] part=new byte[chunk];
        for (int n;(n=in.read(part))!=-1;) { check(n>0,"Nonempty read made no progress"); bytes.write(part,0,n); }
        return bytes.toByteArray();
    }
    private static final class Source extends ByteArrayInputStream {
        final int chunk; int closes,reads;
        Source(byte[] bytes,int chunk) { super(bytes); this.chunk=chunk; }
        @Override public synchronized int read(byte[] b,int o,int n) {
            reads++; return super.read(b,o,Math.min(n,chunk));
        }
        @Override public void close() { closes++; }
    }
    private static long nativeHandle(BrotliInputStream stream) throws Exception {
        java.lang.reflect.Field owner=BrotliInputStream.class.getDeclaredField("owner");
        owner.setAccessible(true); Object state=owner.get(stream);
        java.lang.reflect.Field handle=state.getClass().getDeclaredField("handle");
        handle.setAccessible(true); return handle.getLong(state);
    }
    /** Only the source boundary is controlled: the actual native decoder must
     * publish a flushed prefix before its peer sends the final compressed bytes. */
    private static final class DuplexSource extends InputStream {
        final CountDownLatch blocked=new CountDownLatch(1),release=new CountDownLatch(1);
        byte[] current=HttpBrotliFixtures.FLUSH_PING;
        int offset; boolean tail; volatile boolean closed;
        @Override public int read() throws IOException { byte[] b=new byte[1]; int n=read(b); return n<0?-1:b[0]&255; }
        @Override public int read(byte[] b,int o,int n) throws IOException {
            if (offset==current.length && !tail) {
                blocked.countDown();
                try { if(!release.await(10,TimeUnit.SECONDS)) throw new IOException("Duplex source timed out"); }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException(e); }
                if(closed) throw new IOException("Closed by cancellation");
                current=HttpBrotliFixtures.FINISH_PONG; offset=0; tail=true;
            }
            if(closed) throw new IOException("Closed by cancellation");
            if(offset==current.length) return -1;
            int count=Math.min(n,current.length-offset);
            System.arraycopy(current,offset,b,o,count); offset+=count; return count;
        }
        @Override public void close() { closed=true; release.countDown(); }
    }
    private static void nativeContracts() throws Exception {
        check(java.util.HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(PAYLOAD))
                .equals(HttpBrotliFixtures.PAYLOAD_SHA256),"Fixture payload identity");
        for(int input:new int[]{1,2,7,32768}) for(int output:new int[]{1,3,32,65536}) {
            Source raw=new Source(HttpBrotliFixtures.PING,input);
            BrotliInputStream stream=new BrotliInputStream(raw);
            check(raw.reads==0,"Decoder constructor read a duplex body");
            check(Arrays.equals(read(stream,output),new byte[]{'p','i','n','g'}),"Split native decoding");
            check(nativeHandle(stream)==0,"EOF retained native decoder");
            stream.close(); stream.close(); check(raw.closes==1,"Native source closed more than once");
        }
        for(int chunk:new int[]{127,32768}) {
            Source raw=new Source(HttpBrotliFixtures.RECORDS,chunk);
            try(BrotliInputStream stream=new BrotliInputStream(raw)) {
                check(Arrays.equals(read(stream,chunk),PAYLOAD),"Large compressed response mismatch");
            }
        }
        try(BrotliInputStream empty=new BrotliInputStream(new ByteArrayInputStream(HttpBrotliFixtures.EMPTY))) {
            check(empty.read()==-1 && empty.read()==-1,"Encoded empty body EOF");
        }
        for(int length=0;length<HttpBrotliFixtures.PING.length;length++) {
            Source raw=new Source(Arrays.copyOf(HttpBrotliFixtures.PING,length),1);
            BrotliInputStream stream=new BrotliInputStream(raw);
            rejects(IOException.class,()->read(stream,3));
            check(nativeHandle(stream)==0 && raw.closes==1,"Truncation leaked native/source ownership");
        }
        Source invalid=new Source(new byte[]{-1,-1,-1,-1,-1,-1,-1,-1},3);
        BrotliInputStream bad=new BrotliInputStream(invalid);
        rejects(IOException.class,()->read(bad,32));
        check(nativeHandle(bad)==0 && invalid.closes==1,"Malformed body leaked native ownership");
        BrotliInputStream closed=new BrotliInputStream(new ByteArrayInputStream(HttpBrotliFixtures.PING));
        rejects(IndexOutOfBoundsException.class,()->closed.read(new byte[8],7,2));
        check(closed.read(new byte[0])==0,"Empty read consumed native input");
        closed.close(); check(nativeHandle(closed)==0,"Early close retained decoder");
        rejects(IOException.class,closed::read);
        ExecutorService executor=Executors.newSingleThreadExecutor();
        try {
            DuplexSource source=new DuplexSource();
            try(BrotliInputStream stream=new BrotliInputStream(source)) {
                byte[] prefix=new byte[32];
                int n=executor.submit(()->stream.read(prefix)).get(3,TimeUnit.SECONDS);
                check(n==4 && new String(prefix,0,n,StandardCharsets.UTF_8).equals("ping"),"Prefix was buffered until EOF");
                check(source.blocked.getCount()==1,"Decoder read beyond available decoded prefix");
                source.release.countDown();
                check(new String(read(stream,32),StandardCharsets.UTF_8).equals("pong"),"Flush/finish continuity");
            }
            DuplexSource source2=new DuplexSource();
            BrotliInputStream stream2=new BrotliInputStream(source2);
            check(stream2.read(new byte[32])==4,"Cancellation prefix");
            Future<?> waiting=executor.submit(()->{try {stream2.read(new byte[32]); throw new AssertionError("Canceled read succeeded");}
                catch(IOException expected) {}});
            check(source2.blocked.await(3,TimeUnit.SECONDS),"Native reader did not wait for peer");
            stream2.close(); waiting.get(3,TimeUnit.SECONDS);
            check(nativeHandle(stream2)==0,"Concurrent cancel retained decoder");
        } finally { executor.shutdownNow(); check(executor.awaitTermination(3,TimeUnit.SECONDS),"Native test reader leaked"); }
        byte[] trailing=Arrays.copyOf(HttpBrotliFixtures.PING,HttpBrotliFixtures.PING.length+100000);
        Source source=new Source(trailing,97);
        InputStream decoded=ContentDecoding.wrap(source,"br",true);
        check(Arrays.equals(read(decoded,3),new byte[]{'p','i','n','g'}),"Trailing encoded data behavior");
        check(source.available()==0 && ContentDecoding.encodedBytes(decoded)==trailing.length,"Trailing bytes did not reach framing EOF/accounting");
        decoded.close();
        for(String encoding:new String[]{"br,unknown","identity,br","br,identity","br,"}) {
            Source raw=new Source(HttpBrotliFixtures.PING,32768);
            check(ContentDecoding.wrap(raw,encoding,true)==raw,"Partially decoded unknown coding chain");
        }
        Source chain=new Source(gzip(HttpBrotliFixtures.RECORDS),32768);
        try(InputStream stream=ContentDecoding.wrap(chain,"br, gzip",true)) {
            check(Arrays.equals(read(stream,8192),PAYLOAD),"Content coding order");
        }
        check(chain.closes==1,"Layered decoder retired raw source more than once");
        rejects(IOException.class,()->ContentDecoding.wrap(new ByteArrayInputStream(new byte[0]),String.join(",",Collections.nCopies(11,"br")),true));
    }
    private static void policyContracts() throws Exception {
        for(String url:new String[]{"https://example.com/","http://localhost/","http://child.localhost/",
                "http://127.0.0.1/","http://127.4.3.2/","http://[::1]/"}) {
            check(ContentDecoding.acceptEncoding(new URL(url),false).equals("gzip, deflate, br"),"Secure/local negotiation: "+url);
            check(ContentDecoding.acceptEncoding(new URL(url),true).equals("identity"),"Range negotiation");
        }
        for(String url:new String[]{"http://example.com/","http://notlocalhost/","http://localhost.example/",
                "http://127.999.0.1/","http://128.0.0.1/"})
            check(ContentDecoding.acceptEncoding(new URL(url),false).equals("gzip, deflate"),"Insecure negotiation: "+url);
        HttpEngine.Builder builder=new HttpEngine.Builder(new android.content.ContextWrapper(null));
        JavaHttpEngine disabled=(JavaHttpEngine)builder.build();
        JavaHttpEngine enabled=(JavaHttpEngine)builder.setEnableBrotli(true).build();
        builder.setEnableBrotli(false);
        try {
            check(!disabled.brotliEnabled && enabled.brotliEnabled,"Engine policy was not snapshotted/default disabled");
            List<Map.Entry<String,String>> explicit=Collections.singletonList(new AbstractMap.SimpleImmutableEntry<>("aCcEpT-EnCoDiNg","identity"));
            check(enabled.contentHeaders(new URL("http://localhost/"),explicit)==explicit,"Explicit coding was replaced");
            List<Map.Entry<String,String>> empty=Collections.emptyList();
            check(disabled.contentHeaders(new URL("http://localhost/"),empty)==empty,"Disabled engine advertised Brotli");
            check(enabled.contentHeaders(new URL("http://localhost/"),empty).size()==1 && empty.isEmpty(),"Added policy mutated public request headers");
        } finally { disabled.shutdown(); enabled.shutdown(); }
    }
    private static final class Peer implements AutoCloseable {
        final ServerSocket server=new ServerSocket(0,16,InetAddress.getByName("127.0.0.1"));
        final ExecutorService workers=Executors.newCachedThreadPool();
        final ConcurrentLinkedQueue<Throwable> failures=new ConcurrentLinkedQueue<>();
        final ConcurrentHashMap<String,AtomicInteger> requests=new ConcurrentHashMap<>();
        final ConcurrentHashMap<String,String> accepted=new ConcurrentHashMap<>();
        final AtomicLong bodyBytes=new AtomicLong();
        final Thread accept;
        final byte[] gzip=gzip(PAYLOAD);
        final CountDownLatch slowHeaders=new CountDownLatch(1),slowRelease=new CountDownLatch(1);
        volatile boolean closed;
        Peer() throws IOException {
            accept=new Thread(()->{try {while(!closed) {Socket s=server.accept(); workers.execute(()->serve(s));}}
                catch(Throwable failure) {if(!closed) failures.add(failure);}},"brotli-peer");
            accept.setDaemon(true); accept.start();
        }
        URL url(String path) throws Exception {return new URL("http://127.0.0.1:"+server.getLocalPort()+path);}
        int requests(String path) {AtomicInteger n=requests.get(path); return n==null?0:n.get();}
        void serve(Socket socket) {
            String path="";
            try(Socket s=socket) {
                s.setSoTimeout(10000);
                BufferedReader in=new BufferedReader(new InputStreamReader(s.getInputStream(),StandardCharsets.US_ASCII));
                String line=in.readLine(); if(line==null)return;
                String method=line.split(" ")[0]; path=line.split(" ")[1];
                Map<String,String> headers=new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
                for(String h;(h=in.readLine())!=null && !h.isEmpty();) {
                    int colon=h.indexOf(':'); headers.put(h.substring(0,colon),h.substring(colon+1).trim());
                }
                requests.computeIfAbsent(path,k->new AtomicInteger()).incrementAndGet();
                String coding=headers.getOrDefault("Accept-Encoding",""); accepted.put(path,coding);
                boolean force=path.startsWith("/force") || path.startsWith("/short") || path.startsWith("/slow");
                String encoding=force || coding.contains("br") ? "br" : coding.contains("gzip") ? "gzip" : "";
                byte[] body=encoding.equals("br") ? HttpBrotliFixtures.RECORDS : encoding.equals("gzip") ? gzip : PAYLOAD;
                if(path.startsWith("/chain")) {body=gzip(HttpBrotliFixtures.RECORDS);encoding="br, gzip";}
                if(path.startsWith("/short")) body=Arrays.copyOf(body,body.length/2);
                if(path.startsWith("/trailing")) body=Arrays.copyOf(body,body.length+123);
                int status=path.startsWith("/missing")?404:path.startsWith("/empty")?204:200;
                if(status==204) body=new byte[0];
                OutputStream out=s.getOutputStream();
                out.write(("HTTP/1.1 "+status+" Result\r\nContent-Length: "+body.length
                        +"\r\nCache-Control: max-age=600\r\nVary: Accept-Encoding\r\n"
                        +(encoding.isEmpty()?"":"Content-Encoding: "+encoding+"\r\n")
                        +"Connection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII)); out.flush();
                if(path.startsWith("/slow")) {
                    slowHeaders.countDown();
                    if(!slowRelease.await(10,TimeUnit.SECONDS)) throw new IOException("Body withheld until header callback");
                }
                if(!method.equals("HEAD")) {out.write(body);out.flush();bodyBytes.addAndGet(body.length);}
            } catch(Throwable failure) {if(!closed && !path.startsWith("/slow"))failures.add(failure);}
        }
        @Override public void close() throws Exception {
            closed=true;slowRelease.countDown();server.close();accept.join(3000);
            workers.shutdownNow();check(workers.awaitTermination(5,TimeUnit.SECONDS),"Peer workers retained");
            check(failures.isEmpty(),"Peer failed: "+failures);
        }
    }
    private static final class Result {
        final byte[] body;final UrlResponseInfo info;
        Result(byte[] body,UrlResponseInfo info) {this.body=body;this.info=info;}
    }
    private static Result request(HttpEngine engine,Peer peer,String path,ExecutorService executor,
                                  String method,String... headers) throws Exception {
        CompletableFuture<Result> done=new CompletableFuture<>();
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        AtomicInteger terminals=new AtomicInteger();
        UrlRequest.Builder builder=engine.newUrlRequestBuilder(peer.url(path).toString(),executor,new UrlRequest.Callback() {
            public void onRedirectReceived(UrlRequest r,UrlResponseInfo i,String target) {r.followRedirect();}
            public void onResponseStarted(UrlRequest r,UrlResponseInfo i) {
                if(path.startsWith("/slow"))peer.slowRelease.countDown();
                r.read(ByteBuffer.allocateDirect(8191));
            }
            public void onReadCompleted(UrlRequest r,UrlResponseInfo i,ByteBuffer buffer) {
                buffer.flip();byte[] data=new byte[buffer.remaining()];buffer.get(data);bytes.write(data,0,data.length);
                buffer.clear();r.read(buffer);
            }
            public void onSucceeded(UrlRequest r,UrlResponseInfo i) {terminals.incrementAndGet();done.complete(new Result(bytes.toByteArray(),i));}
            public void onFailed(UrlRequest r,UrlResponseInfo i,HttpException e) {terminals.incrementAndGet();done.completeExceptionally(e);}
            public void onCanceled(UrlRequest r,UrlResponseInfo i) {terminals.incrementAndGet();done.completeExceptionally(new AssertionError("Unexpected cancel"));}
        }).setHttpMethod(method);
        for(int i=0;i<headers.length;i+=2)builder.addHeader(headers[i],headers[i+1]);
        UrlRequest request=builder.build();request.start();
        try {
            Result result=done.get(20,TimeUnit.SECONDS);
            check(terminals.get()==1 && request.isDone(),"Terminal request ownership");return result;
        } catch(Exception failure) {request.cancel();throw failure;}
    }
    private static HttpEngine engine(boolean brotli,boolean cache) {
        HttpEngine.Builder b=new HttpEngine.Builder(new android.content.ContextWrapper(null)).setEnableBrotli(brotli);
        if(cache)b.setEnableHttpCache(HttpEngine.Builder.HTTP_CACHE_IN_MEMORY,4*1024*1024);
        return b.build();
    }
    private static void httpContracts() throws Exception {
        ExecutorService callbacks=Executors.newSingleThreadExecutor();
        HttpEngine enabled=engine(true,true),disabled=engine(false,false);
        try(Peer peer=new Peer()) {
            Result first=request(enabled,peer,"/records",callbacks,"GET");
            check(Arrays.equals(first.body,PAYLOAD),"Unary Brotli bytes");
            check(first.info.getReceivedByteCount()==HttpBrotliFixtures.RECORDS.length,"Decoded bytes counted as wire bytes");
            check(first.info.getHeaders().getAsMap().get("Content-Encoding").equals(Collections.singletonList("br")),"Raw content coding header lost");
            check(peer.accepted.get("/records").equals("gzip, deflate, br"),"Enabled engine did not negotiate Brotli");
            Result cached=request(enabled,peer,"/records",callbacks,"GET");
            check(cached.info.wasCached() && cached.info.getReceivedByteCount()==0 && peer.requests("/records")==1,"Compressed cache entry was not committed/reused");
            check(Arrays.equals(cached.body,PAYLOAD),"Cache stored decoded body under encoded headers");
            check(Arrays.equals(request(disabled,peer,"/baseline",callbacks,"GET").body,PAYLOAD),"Default gzip behavior changed");
            check(!peer.accepted.get("/baseline").contains("br"),"Disabled engine advertised Brotli");
            check(Arrays.equals(request(disabled,peer,"/force",callbacks,"GET").body,HttpBrotliFixtures.RECORDS),"Disabled engine decoded unsolicited Brotli");
            check(Arrays.equals(request(enabled,peer,"/explicit",callbacks,"GET","aCcEpT-EnCoDiNg","identity").body,PAYLOAD),"Explicit coding response changed");
            check(peer.accepted.get("/explicit").equals("identity"),"Explicit coding replaced");
            check(Arrays.equals(request(enabled,peer,"/range",callbacks,"GET","Range","bytes=0-99").body,PAYLOAD),"Legal full response to Range changed");
            check(peer.accepted.get("/range").equals("identity"),"Range implicitly compressed");
            check(Arrays.equals(request(enabled,peer,"/chain",callbacks,"GET").body,PAYLOAD),"Unary stacked coding order");
            check(Arrays.equals(request(enabled,peer,"/slow",callbacks,"GET").body,PAYLOAD),"Header callback waited for decoded body");
            check(request(enabled,peer,"/head",callbacks,"HEAD").body.length==0,"HEAD decoded nonexistent body");
            check(request(enabled,peer,"/empty",callbacks,"GET").body.length==0,"204 decoded nonexistent body");
            Result error=request(enabled,peer,"/missing",callbacks,"GET");
            check(error.info.getHttpStatusCode()==404 && Arrays.equals(error.body,PAYLOAD),"Encoded error body changed");
            for(int attempt=0;attempt<2;attempt++) rejects(ExecutionException.class,()->request(enabled,peer,"/short",callbacks,"GET"));
            // The provider may cache a complete encoded HTTP body before a
            // decoder detects corruption. It must still fail on every read.
            for(String path:new String[]{"/connection","/trailing"}) {
                HttpURLConnection connection=(HttpURLConnection)enabled.openConnection(peer.url(path));
                try(InputStream in=connection.getInputStream()) {
                    check(Arrays.equals(read(in,32768),PAYLOAD),"URLConnection decoded body "+path);
                    check(connection.getHeaderField("Content-Encoding").equals("br"),"URLConnection raw metadata");
                } finally {connection.disconnect();}
                Result repeat=request(enabled,peer,path,callbacks,"GET");
                check(repeat.info.wasCached() && peer.requests(path)==1 && Arrays.equals(repeat.body,PAYLOAD),"URLConnection/framing cache lifetime "+path);
            }
            long gzipBytes=peer.gzip.length,brBytes=HttpBrotliFixtures.RECORDS.length;
            check(brBytes*100<gzipBytes*60,"Brotli fixture no longer removes at least40% of gzip wire bytes");
            System.out.println("BROTLI_WIRE_WORK payload="+PAYLOAD.length+" gzip="+gzipBytes+" br="+brBytes
                    +" cachedNetworkBytes="+cached.info.getReceivedByteCount());
        } finally {enabled.shutdown();disabled.shutdown();callbacks.shutdownNow();
            check(callbacks.awaitTermination(5,TimeUnit.SECONDS),"HTTP callback worker retained");}
    }
    public static void main(String[] args) throws Exception {
        if(Arrays.asList(args).contains("--host"))System.setProperty("java.protocol.handler.pkgs","gesher.http.policy");
        nativeContracts();policyContracts();httpContracts();
        System.out.println("HttpEngineBrotliTest passed: "+checks+" native/HTTP contracts");
    }
}
