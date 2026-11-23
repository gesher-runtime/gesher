// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import android.net.http.*;
import java.io.*;
import java.net.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.zip.GZIPOutputStream;

/** Public API only, so the complete unchanged pre-feature engine can run the
 * identical traffic workload. Peer sends gzip or Brotli from actual negotiation. */
public final class HttpEngineBrotliWireTest {
    private static byte[] payload() {
        StringBuilder value=new StringBuilder();
        for(int i=0;i<8192;i++)value.append("{\"id\":").append(i)
                .append(",\"title\":\"Android application record ").append(i%73)
                .append("\",\"enabled\":true,\"description\":\"A real incremental HTTP response keeps callbacks and cancellation responsive.\"}\n");
        return value.toString().getBytes(StandardCharsets.UTF_8);
    }
    public static void main(String[] args) throws Exception {
        boolean baseline=Arrays.asList(args).contains("--baseline");
        System.setProperty("java.protocol.handler.pkgs","gesher.http.policy");
        byte[] plain=payload();
        ByteArrayOutputStream encoded=new ByteArrayOutputStream();
        try(GZIPOutputStream gzip=new GZIPOutputStream(encoded)) {gzip.write(plain);}
        byte[] gzip=encoded.toByteArray(),brotli=HttpBrotliFixtures.RECORDS;
        ExecutorService callbacks=Executors.newSingleThreadExecutor();
        HttpEngine engine=new HttpEngine.Builder(new android.content.ContextWrapper(null)).setEnableBrotli(true).build();
        AtomicLong wireBytes=new AtomicLong();
        AtomicInteger brResponses=new AtomicInteger(),requests=new AtomicInteger();
        CompletableFuture<Void> peerDone=new CompletableFuture<>();
        try(ServerSocket listener=new ServerSocket(0,16,InetAddress.getByName("127.0.0.1"))) {
            listener.setSoTimeout(15000);
            Thread peer=new Thread(()->{
                try {
                    for(int request=0;request<10;request++)try(Socket s=listener.accept()) {
                        s.setSoTimeout(10000);
                        BufferedReader in=new BufferedReader(new InputStreamReader(s.getInputStream(),StandardCharsets.US_ASCII));
                        String first=in.readLine();
                        if(first==null || !first.startsWith("GET /records"))throw new IOException("Unexpected request");
                        String coding="";
                        for(String h;(h=in.readLine())!=null && !h.isEmpty();) {
                            int colon=h.indexOf(':');
                            if(h.substring(0,colon).equalsIgnoreCase("Accept-Encoding"))coding=h.substring(colon+1).trim();
                        }
                        boolean acceptsBrotli=Arrays.asList(coding.split("\\s*,\\s*")).contains("br");
                        byte[] body=acceptsBrotli?brotli:gzip;
                        if(acceptsBrotli)brResponses.incrementAndGet();
                        OutputStream out=s.getOutputStream();
                        out.write(("HTTP/1.1 200 OK\r\nContent-Encoding: "+(acceptsBrotli?"br":"gzip")
                                +"\r\nContent-Length: "+body.length+"\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n")
                                .getBytes(StandardCharsets.US_ASCII));
                        out.write(body);out.flush();wireBytes.addAndGet(body.length);requests.incrementAndGet();
                    }
                    peerDone.complete(null);
                } catch(Throwable failure) {peerDone.completeExceptionally(failure);}
            },"brotli-wire-peer");peer.setDaemon(true);peer.start();
            for(int n=0;n<10;n++) {
                CompletableFuture<Void> complete=new CompletableFuture<>();
                MessageDigest digest=MessageDigest.getInstance("SHA-256");
                AtomicInteger decoded=new AtomicInteger();
                UrlRequest request=engine.newUrlRequestBuilder("http://127.0.0.1:"+listener.getLocalPort()+"/records/"+n,
                        callbacks,new UrlRequest.Callback() {
                    public void onRedirectReceived(UrlRequest r,UrlResponseInfo i,String target) {complete.completeExceptionally(new AssertionError("Unexpected redirect"));r.cancel();}
                    public void onResponseStarted(UrlRequest r,UrlResponseInfo i) {r.read(ByteBuffer.allocateDirect(32768));}
                    public void onReadCompleted(UrlRequest r,UrlResponseInfo i,ByteBuffer b) {
                        b.flip();decoded.addAndGet(b.remaining());digest.update(b);b.clear();r.read(b);
                    }
                    public void onSucceeded(UrlRequest r,UrlResponseInfo i) {
                        if(decoded.get()!=plain.length || !Arrays.equals(digest.digest(),sha256(plain)))
                            complete.completeExceptionally(new AssertionError("Payload changed"));
                        else complete.complete(null);
                    }
                    public void onFailed(UrlRequest r,UrlResponseInfo i,HttpException failure) {complete.completeExceptionally(failure);}
                    public void onCanceled(UrlRequest r,UrlResponseInfo i) {complete.completeExceptionally(new AssertionError("Unexpected cancellation"));}
                }).build();
                request.start();
                try {complete.get(15,TimeUnit.SECONDS);} catch(Exception failure) {request.cancel();throw failure;}
            }
            peerDone.get(5,TimeUnit.SECONDS);peer.join(5000);
            if(requests.get()!=10 || brResponses.get()!=(baseline?0:10))throw new AssertionError("Unexpected content negotiation");
            long expected=10L*(baseline?gzip.length:brotli.length);
            if(wireBytes.get()!=expected || !baseline && expected>=10L*gzip.length*0.60)
                throw new AssertionError("Encoded traffic budget failed");
            System.out.println("BROTLI_PUBLIC_WORKLOAD requests="+requests.get()+" decoded="+(10L*plain.length)
                    +" wire="+wireBytes.get()+" brotliResponses="+brResponses.get()+" baseline="+baseline);
        } finally {engine.shutdown();callbacks.shutdownNow();}
    }
    private static byte[] sha256(byte[] data) {
        try {return MessageDigest.getInstance("SHA-256").digest(data);}
        catch(java.security.NoSuchAlgorithmException impossible) {throw new AssertionError(impossible);}
    }
}
