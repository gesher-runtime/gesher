// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import android.net.Network;
import java.io.*;
import java.net.*;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** URLConnection supplies proxy selection, Android network security config and TLS. */
final class JavaHttpEngine extends HttpEngine {
    // Capture the platform handlers before an engine's factory can be installed.
    // Explicit requests must not inherit another engine's protocol/network policy
    // or depend on that engine remaining open after global factory installation.
    private static final URL HTTP_BASE = transportBase("http://localhost/");
    private static final URL HTTPS_BASE = transportBase("https://localhost/");
    final String agent;
    final boolean http2Enabled;
    final Http2SessionPool http2Sessions = new Http2SessionPool();
    private final EngineHttpCache cache;
    final ExecutorService io = Executors.newCachedThreadPool(r -> {
        Thread thread = new Thread(r, "Gesher-HttpEngine"); thread.setDaemon(true); return thread;
    });
    private final Set<Object> active = new HashSet<>();
    private boolean closed;
    private Network network;

    JavaHttpEngine(String agent, boolean http2Enabled, EngineHttpCache cache) {
        this.agent = agent;
        this.http2Enabled = http2Enabled;
        this.cache = cache;
    }
    synchronized void checkOpen() {
        if (closed) throw new IllegalStateException("HttpEngine is shut down");
    }
    @Override public synchronized void bindToNetwork(Network value) { checkOpen(); network=value; }
    @Override public synchronized URLConnection openConnection(URL url) throws IOException {
        checkOpen(); checkUrl(url);
        URLConnection connection = openTransport(url, network);
        boolean androidConnection = isAndroidHttpConnection(connection);
        if (androidConnection) configureProtocols(connection);
        cache.configure((HttpURLConnection) connection, false);
        connection.setRequestProperty("User-Agent", agent);
        // HttpEngine exposes HTTP request properties, not the provider's private
        // protocol setters. Keep a returned connection from changing engine policy.
        return androidConnection ? new JavaEngineConnection(this, (HttpURLConnection) connection) : connection;
    }
    private static URL transportBase(String value) {
        try { return new URL(value); }
        catch (MalformedURLException e) { throw new ExceptionInInitializerError(e); }
    }
    private static URLConnection openTransport(URL url, Network network) throws IOException {
        URL base = url.getProtocol().equals("http") ? HTTP_BASE : HTTPS_BASE;
        URL target = new URL(base, url.toExternalForm());
        return network == null ? target.openConnection() : network.openConnection(target);
    }

    private static boolean isAndroidHttpConnection(URLConnection connection) {
        String name = connection.getClass().getName();
        return name.equals("com.android.okhttp.internal.huc.HttpURLConnectionImpl")
                || name.equals("com.android.okhttp.internal.huc.HttpsURLConnectionImpl");
    }

    private void configureProtocols(URLConnection connection) {
        // Android's repackaged OkHttp consumes this property locally. Its
        // pool includes the protocol list in the address key, separating
        // HTTP/2-enabled connections from HTTP/1.1-only connections.
        connection.setRequestProperty("X-Android-Protocols", http2Enabled ? "h2,http/1.1" : "http/1.1");
    }

    static String transportHeaderName(String name) {
        // OkHttp consumes these two exact spellings as local control properties.
        // HTTP field names are case-insensitive; use their ordinary wire spelling
        // so application metadata remains present without changing transport policy.
        if ("X-Android-Protocols".equals(name)) return "x-android-protocols";
        if ("X-Android-Transports".equals(name)) return "x-android-transports";
        return name;
    }

    @Override public URLStreamHandlerFactory createUrlStreamHandlerFactory() {
        return protocol -> {
            if (!protocol.equals("http") && !protocol.equals("https")) return null;
            return new URLStreamHandler() {
                @Override protected URLConnection openConnection(URL url) throws IOException {
                    return JavaHttpEngine.this.openConnection(url);
                }
                @Override protected int getDefaultPort() { return protocol.equals("http") ? 80 : 443; }
            };
        };
    }
    @Override public synchronized UrlRequest.Builder newUrlRequestBuilder(String url, Executor executor, UrlRequest.Callback callback) {
        checkOpen(); return new RequestBuilder(this, url, executor, callback, network);
    }
    @Override public synchronized BidirectionalStream.Builder newBidirectionalStreamBuilder(String url, Executor executor, BidirectionalStream.Callback callback) {
        checkOpen();
        return new JavaBidirectionalStream.Builder(this,url,executor,callback,network);
    }
    @Override public synchronized void shutdown() {
        if (!active.isEmpty()) throw new IllegalStateException("Cannot shut down an engine with active requests");
        if (closed) return;
        closed=true; http2Sessions.close(); io.shutdown(); cache.close();
    }
    synchronized void register(Object request) { checkOpen(); active.add(request); }
    synchronized void release(Object request) { active.remove(request); }
    private static void checkUrl(URL url) {
        if ((!url.getProtocol().equals("http") && !url.getProtocol().equals("https")) || url.getHost().isEmpty())
            throw new IllegalArgumentException("Only HTTP(S) URLs are supported");
    }
    static URL url(String value) {
        try { URL result=new URL(Objects.requireNonNull(value)); checkUrl(result); return result; }
        catch (MalformedURLException e) { throw new IllegalArgumentException("Invalid URL", e); }
    }
    static String token(String value) {
        Objects.requireNonNull(value);
        if (!value.matches("[!#$%&'*+.^_`|~0-9A-Za-z-]+")) throw new IllegalArgumentException("Invalid HTTP token");
        return value;
    }
    static void header(List<Map.Entry<String,String>> headers, String name, String value) {
        token(name); Objects.requireNonNull(value);
        for (int i=0;i<value.length();i++) {
            char ch=value.charAt(i);
            if (ch=='\r' || ch=='\n' || ch==0 || ch==127 || (ch<32 && ch!='\t'))
                throw new IllegalArgumentException("Invalid HTTP header value");
        }
        headers.add(new AbstractMap.SimpleImmutableEntry<>(name,value));
    }
    static final class Headers extends HeaderBlock {
        private final List<Map.Entry<String,String>> list;
        private final Map<String,List<String>> map;
        Headers(List<Map.Entry<String,String>> values) {
            List<Map.Entry<String,String>> copy=new ArrayList<>();
            Map<String,List<String>> grouped=new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
            for (Map.Entry<String,String> entry: values) {
                copy.add(new AbstractMap.SimpleImmutableEntry<>(entry));
                grouped.computeIfAbsent(entry.getKey(), k -> new ArrayList<>()).add(entry.getValue());
            }
            grouped.replaceAll((k,v) -> Collections.unmodifiableList(v));
            list=Collections.unmodifiableList(copy); map=Collections.unmodifiableMap(grouped);
        }
        @Override public List<Map.Entry<String,String>> getAsList() { return list; }
        @Override public Map<String,List<String>> getAsMap() { return map; }
    }
    private static final class Response extends UrlResponseInfo {
        final List<String> chain;
        final int code;
        final String message;
        final String protocol;
        final Headers headers;
        final boolean cached;
        volatile long received;
        Response(List<String> chain, HttpURLConnection connection) throws IOException {
            this.chain=Collections.unmodifiableList(new ArrayList<>(chain));
            code=connection.getResponseCode(); message=Objects.toString(connection.getResponseMessage(), "");
            protocol = isAndroidHttpConnection(connection)
                    ? Objects.toString(connection.getHeaderField("X-Android-Selected-Protocol"), "http/1.1")
                    : "http/1.1";
            cached = isAndroidHttpConnection(connection) && EngineHttpCache.wasCached(connection);
            List<Map.Entry<String,String>> values=new ArrayList<>();
            // Android OkHttp starts real fields at zero; other providers put the
            // unnamed status line there. Preserve both layouts without losing a field.
            for (int i=0;;i++) {
                String name=connection.getHeaderFieldKey(i), value=connection.getHeaderField(i);
                if (name==null && value==null) break;
                if (name!=null && value!=null) values.add(new AbstractMap.SimpleImmutableEntry<>(name,value));
            }
            headers=new Headers(values);
        }
        @Override public HeaderBlock getHeaders() { return headers; }
        @Override public int getHttpStatusCode() { return code; }
        @Override public String getHttpStatusText() { return message; }
        @Override public String getNegotiatedProtocol() { return protocol; }
        @Override public long getReceivedByteCount() { return received; }
        @Override public String getUrl() { return chain.get(chain.size()-1); }
        @Override public List<String> getUrlChain() { return chain; }
        @Override public boolean wasCached() { return cached; }
    }
    private static final class RequestBuilder extends UrlRequest.Builder {
        final JavaHttpEngine engine;
        final String url;
        final Executor executor;
        final UrlRequest.Callback callback;
        final List<Map.Entry<String,String>> headers=new ArrayList<>();
        Network network;
        String method;
        int priority=UrlRequest.REQUEST_PRIORITY_MEDIUM, tag, uid;
        boolean cacheDisabled, direct, hasTag, hasUid;
        UploadDataProvider upload;
        Executor uploadExecutor;
        RequestBuilder(JavaHttpEngine engine, String url, Executor executor, UrlRequest.Callback callback, Network network) {
            this.engine=engine; JavaHttpEngine.url(url); this.url=url;
            this.executor=Objects.requireNonNull(executor); this.callback=Objects.requireNonNull(callback); this.network=network;
        }
        @Override public UrlRequest.Builder addHeader(String name, String value) { header(headers,name,value); return this; }
        @Override public UrlRequest.Builder bindToNetwork(Network value) { network=value; return this; }
        @Override public UrlRequest.Builder setCacheDisabled(boolean value) { cacheDisabled=value; return this; }
        @Override public UrlRequest.Builder setDirectExecutorAllowed(boolean value) { direct=value; return this; }
        @Override public UrlRequest.Builder setHttpMethod(String value) { method=token(value); return this; }
        @Override public UrlRequest.Builder setPriority(int value) {
            if (value<0 || value>4) throw new IllegalArgumentException("Invalid request priority");
            priority=value; return this;
        }
        @Override public UrlRequest.Builder setTrafficStatsTag(int value) { tag=value; hasTag=true; return this; }
        @Override public UrlRequest.Builder setTrafficStatsUid(int value) { uid=value; hasUid=true; return this; }
        @Override public UrlRequest.Builder setUploadDataProvider(UploadDataProvider value, Executor executor) {
            upload=Objects.requireNonNull(value); uploadExecutor=Objects.requireNonNull(executor); return this;
        }
        @Override public UrlRequest build() {
            engine.checkOpen();
            if (upload!=null && headers.stream().noneMatch(h -> h.getKey().equalsIgnoreCase("Content-Type")))
                throw new IllegalArgumentException("Upload requests require a Content-Type header");
            return new Request(this);
        }
    }
    /** Serialize application callbacks even when their executor is a thread pool. */
    static final class SerialExecutor implements Executor {
        private static final class RejectionAware implements Runnable {
            final Runnable action;
            final java.util.function.Consumer<RuntimeException> rejected;
            RejectionAware(Runnable action, java.util.function.Consumer<RuntimeException> rejected) {
                this.action=action; this.rejected=rejected;
            }
            @Override public void run() { action.run(); }
        }
        final Executor target;
        final ArrayDeque<Runnable> tasks=new ArrayDeque<>();
        boolean running;
        SerialExecutor(Executor target) { this.target=target; }
        @Override public synchronized void execute(Runnable command) {
            tasks.add(command);
            if (!running) { running=true; schedule(); }
        }
        void execute(Runnable command, java.util.function.Consumer<RuntimeException> rejected) {
            execute(new RejectionAware(command,rejected));
        }
        private static void discarded(Runnable task, RuntimeException error) {
            if (task instanceof RejectionAware) ((RejectionAware)task).rejected.accept(error);
        }
        private synchronized void schedule() {
            Runnable next=tasks.poll();
            if (next==null) { running=false; return; }
            try { target.execute(() -> { try { next.run(); } finally { schedule(); } }); }
            catch (RuntimeException e) {
                running=false;
                // Rejection can happen later, when a previous callback exits.
                // Notify every dropped terminal owner, not only the caller of
                // the original execute(), which may already have returned.
                discarded(next,e);
                for (Runnable task; (task=tasks.poll())!=null;) discarded(task,e);
                throw e;
            }
        }
    }
    private interface CallbackAction { void run() throws Exception; }
    private static final class Request extends UrlRequest {
        private static final int NEW=0, CONNECTING=1, REDIRECT=2, READY=3, READING=4, DONE=5;
        final JavaHttpEngine engine;
        final Callback callback;
        final SerialExecutor callbacks;
        final SerialExecutor uploads;
        final Headers headers;
        final List<Map.Entry<String,String>> effectiveHeaders;
        final Network network;
        final String initialMethod;
        final int priority, tag, uid;
        final boolean cacheDisabled, direct, hasTag, hasUid;
        final UploadDataProvider provider;
        final AtomicBoolean uploadClosed=new AtomicBoolean();
        final List<String> chain=new ArrayList<>();
        String method;
        URL current, redirect;
        boolean sendUpload;
        volatile int phase=NEW;
        volatile HttpURLConnection connection;
        volatile InputStream input;
        volatile Response response;
        volatile CompletableFuture<?> pendingUpload;
        // One read is admitted at a time. The previous reader has finished using
        // this storage before it publishes READY or invokes an application
        // callback, including a direct/reentrant callback. Keep capacity across
        // short reads and retire it only after all admitted workers have left.
        byte[] readScratch;
        final CompletableFuture<Void> uploadCanceled=new CompletableFuture<>();
        // Every hop remains owned until its stream is closed. A canceled queued
        // redirect must not lose the previous hop merely because connection moved.
        final Map<HttpURLConnection, InputStream> transports=new IdentityHashMap<>();
        int workers; // Includes admitted work that has not entered the executor yet.
        boolean cleanupQueued, terminalPublished;
        final HttpRequestScheduler.Ticket admission;
        HttpException terminalError;
        boolean terminalCanceled;
        Request(RequestBuilder b) {
            engine=b.engine; callback=b.callback; callbacks=new SerialExecutor(b.executor);
            uploads=b.uploadExecutor==null ? null : new SerialExecutor(b.uploadExecutor);
            headers=new Headers(b.headers); effectiveHeaders=new ArrayList<>(headers.getAsList());
            network=b.network; initialMethod=b.method; method=b.method==null ? (b.upload==null ? "GET" : "POST") : b.method;
            current=url(b.url); priority=b.priority; tag=b.tag; uid=b.uid;
            cacheDisabled=b.cacheDisabled; direct=b.direct; hasTag=b.hasTag; hasUid=b.hasUid;
            provider=b.upload; sendUpload=provider!=null;
            admission=new HttpRequestScheduler.Ticket(engine,
                    HttpRequestScheduler.metadata(b.url,engine.agent,headers.getAsList()),
                    priority,() -> work(this::connect));
        }
        @Override public HeaderBlock getHeaders() { return headers; }
        @Override public String getHttpMethod() { return initialMethod; }
        @Override public int getPriority() { return priority; }
        @Override public int getTrafficStatsTag() { return tag; }
        @Override public int getTrafficStatsUid() { return uid; }
        @Override public boolean hasTrafficStatsTag() { return hasTag; }
        @Override public boolean hasTrafficStatsUid() { return hasUid; }
        @Override public boolean isCacheDisabled() { return cacheDisabled; }
        @Override public boolean isDirectExecutorAllowed() { return direct; }
        @Override public boolean isDone() { return phase==DONE; }
        @Override public void getStatus(StatusListener listener) {
            Objects.requireNonNull(listener);
            int state=phase;
            int status=state==NEW || state==DONE ? Status.INVALID : admission.pending
                    ? Status.WAITING_FOR_AVAILABLE_SOCKET : state==CONNECTING ? Status.CONNECTING
                    : state==READING ? Status.READING_RESPONSE : Status.IDLE;
            callbacks.execute(() -> listener.onStatus(status));
        }
        @Override public void start() {
            synchronized (this) {
                if (phase!=NEW) throw new IllegalStateException("Request has already started");
                engine.register(this); phase=CONNECTING;
            }
            IOException unavailable=admission.submit();
            if (unavailable!=null) fail(unavailable);
        }
        private void work(CallbackAction action) {
            synchronized (this) {
                if (isDone()) return;
                workers++;
            }
            try { HttpRequestScheduler.IO.execute(() -> {
                try { if (!isDone()) action.run(); }
                catch (Exception e) { fail(e); }
                finally { workerRetired(); }
            }); } catch (RuntimeException e) {
                try { fail(e); } finally { workerRetired(); }
            }
        }
        private void workerRetired() {
            synchronized (this) { workers--; }
            queueCleanup();
        }
        private void queueCleanup() {
            synchronized (this) {
                if (!isDone() || cleanupQueued || terminalPublished) return;
                cleanupQueued=true;
            }
            // Rejected admission and cancellation before submit own no backend
            // and must not allocate an extra executor task or fallback thread.
            if (!admission.counted) cleanup();
            else HttpRequestScheduler.CLEANUP.execute(this::cleanup);
        }
        private void cleanup() {
            List<HttpURLConnection> cancel;
            synchronized (this) { cancel=new ArrayList<>(transports.keySet()); }
            for (HttpURLConnection c:cancel) try { c.disconnect(); } catch (RuntimeException ignored) {}
            Map<HttpURLConnection,InputStream> owned;
            synchronized (this) {
                cleanupQueued=false;
                if (workers!=0 || terminalPublished) return;
                terminalPublished=true;
                owned=new IdentityHashMap<>(transports);
                transports.clear(); connection=null; input=null; readScratch=null;
            }
            // A connect may have acquired its provider cancel handle after the
            // first pass. No production worker remains, so close it again before
            // closing bodies and releasing the engine-owned cache.
            for (Map.Entry<HttpURLConnection,InputStream> entry:owned.entrySet()) {
                try { entry.getKey().disconnect(); } catch (RuntimeException ignored) {}
                InputStream body=entry.getValue();
                if (body!=null) try { body.close(); } catch (IOException | RuntimeException ignored) {}
            }
            engine.release(this);
            admission.retire();
            // Transport progress and cancellation do not wait for application
            // executors. Keep the outstanding metadata charge until the
            // terminal callback starts (or its executor rejects the handoff).
            if (admission.counted) HttpRequestScheduler.COMPLETION.execute(this::publishTerminal);
            else publishTerminal();
        }
        private void publishTerminal() {
            try { callbacks.execute(() -> {
                admission.release();
                try {
                    if (terminalCanceled) callback.onCanceled(this,response);
                    else if (terminalError!=null) callback.onFailed(this,response,terminalError);
                    else callback.onSucceeded(this,response);
                } catch (RuntimeException ignored) { /* A terminal callback cannot cause another terminal event. */ }
            },error -> admission.release()); } catch (RuntimeException ignored) {
                admission.release(); // A rejected executor cannot receive a callback.
            }
        }
        private void dispatch(CallbackAction action) {
            Thread caller=Thread.currentThread();
            try { callbacks.execute(() -> {
                if (isDone()) return;
                try {
                    if (!direct && Thread.currentThread()==caller) throw new InlineExecutionProhibitedException();
                    action.run();
                } catch (Exception e) { finish(new CallbackException("Exception in request callback", e) {}, false); }
            },this::fail); } catch (RuntimeException e) { fail(e); }
        }
        private void connect() throws Exception {
            URL target=current;
            HttpURLConnection opened = (HttpURLConnection) openTransport(target, network);
            synchronized (this) {
                transports.put(opened,null);
                if (isDone()) return;
                connection=opened;
            }
            opened.setInstanceFollowRedirects(false);
            engine.cache.configure(opened, cacheDisabled);
            opened.setConnectTimeout(30000); opened.setReadTimeout(30000);
            opened.setRequestMethod(method);
            opened.setRequestProperty("User-Agent", engine.agent);
            boolean androidConnection = isAndroidHttpConnection(opened);
            for (Map.Entry<String, String> header : effectiveHeaders) {
                opened.addRequestProperty(androidConnection ? transportHeaderName(header.getKey())
                        : header.getKey(), header.getValue());
            }
            if (androidConnection) engine.configureProtocols(opened);
            if (sendUpload) upload(opened);
            chain.add(target.toExternalForm());
            Response info=new Response(chain,opened);
            response=info;
            InputStream stream=info.code>=400 ? opened.getErrorStream() : opened.getInputStream();
            synchronized (this) { transports.put(opened,stream); }
            String location=opened.getHeaderField("Location");
            if (location!=null && (info.code==301 || info.code==302 || info.code==303 || info.code==307 || info.code==308)) {
                URL next=new URL(target,location); checkUrl(next);
                if (chain.size()>20) throw new ProtocolException("Too many redirects");
                synchronized (this) { if (isDone()) return; redirect=next; phase=REDIRECT; }
                dispatch(() -> callback.onRedirectReceived(this,info,next.toExternalForm()));
                return;
            }
            synchronized (this) {
                if (isDone()) return;
                input=stream==null ? new ByteArrayInputStream(new byte[0]) : stream; phase=READY;
            }
            dispatch(() -> callback.onResponseStarted(this,info));
        }
        @Override public synchronized void followRedirect() {
            if (isDone()) return;
            if (phase!=REDIRECT) throw new IllegalStateException("No redirect is pending");
            URL previous=current; current=redirect; redirect=null; phase=CONNECTING;
            int code=response.code;
            if ((code==303 && !method.equals("HEAD")) || ((code==301 || code==302) && method.equals("POST"))) {
                method="GET"; sendUpload=false;
                effectiveHeaders.removeIf(h -> h.getKey().equalsIgnoreCase("Content-Type") || h.getKey().equalsIgnoreCase("Content-Length"));
            }
            if (!sameOrigin(previous,current)) effectiveHeaders.removeIf(h -> h.getKey().equalsIgnoreCase("Authorization")
                    || h.getKey().equalsIgnoreCase("Cookie") || h.getKey().equalsIgnoreCase("Host"));
            HttpURLConnection old=connection; connection=null;
            work(() -> {
                if (old!=null) {
                    // The provider must see a completed redirect body before
                    // cancellation. Otherwise even an empty cacheable 301 is
                    // aborted instead of committed. OkHttp bounds close/discard
                    // of an unread body; no redirect body reaches the callback.
                    InputStream body;
                    synchronized (this) { body=transports.get(old); }
                    try { if (body!=null) body.close(); }
                    catch (IOException ignored) { /* An unread redirect body is not a response requirement. */ }
                    finally {
                        old.disconnect();
                        synchronized (this) { transports.remove(old); }
                    }
                }
                if (sendUpload) rewind();
                connect();
            });
        }
        private static boolean sameOrigin(URL a, URL b) {
            return a.getProtocol().equalsIgnoreCase(b.getProtocol()) && a.getHost().equalsIgnoreCase(b.getHost())
                    && (a.getPort()<0?a.getDefaultPort():a.getPort())==(b.getPort()<0?b.getDefaultPort():b.getPort());
        }
        @Override public synchronized void read(ByteBuffer buffer) {
            Objects.requireNonNull(buffer);
            if (!buffer.isDirect() || !buffer.hasRemaining() || buffer.isReadOnly())
                throw new IllegalArgumentException("A writable, nonempty direct ByteBuffer is required");
            if (isDone()) return;
            if (phase!=READY) throw new IllegalStateException("Request is not waiting for read()");
            phase=READING;
            work(() -> {
                int capacity=Math.min(buffer.remaining(),32768);
                byte[] bytes=readScratch;
                if (bytes==null || bytes.length<capacity) {
                    int grown=bytes==null ? capacity : Math.max(capacity,Math.min(32768,bytes.length*2));
                    readScratch=bytes=new byte[grown];
                }
                int n=input.read(bytes,0,capacity);
                if (isDone()) return;
                if (n<0) { finish(null,false); return; }
                synchronized (this) {
                    if (isDone()) return;
                    buffer.put(bytes,0,n); if (!response.cached) response.received+=n; phase=READY;
                }
                dispatch(() -> callback.onReadCompleted(this,response,buffer));
            });
        }
        private <T> T await(CompletableFuture<T> future) throws Exception {
            try {
                if (isDone()) throw new CancellationException();
                // Cancel the wait, not the provider's operation. An asynchronous
                // read/rewind still owns its resources until it calls its sink.
                CompletableFuture.anyOf(future,uploadCanceled).get(30,TimeUnit.SECONDS);
                return future.get();
            } catch (ExecutionException e) {
                throw new CallbackException("Exception in upload provider", e.getCause()) {};
            }
        }
        private <T> CompletableFuture<T> uploadFuture() {
            CompletableFuture<T> future=new CompletableFuture<>();
            synchronized (this) {
                if (isDone()) throw new CancellationException();
                pendingUpload=future;
            }
            future.whenComplete((value,error) -> {
                synchronized (this) { if (pendingUpload==future) pendingUpload=null; }
                closeUploadWhenReady();
            });
            return future;
        }
        private void submitUpload(CompletableFuture<?> future, CallbackAction action) {
            try { uploads.execute(() -> {
                try {
                    if (isDone()) throw new CancellationException();
                    action.run();
                } catch (Exception e) { future.completeExceptionally(e); }
            },future::completeExceptionally); } catch (RuntimeException e) { future.completeExceptionally(e); }
        }
        private void closeUploadWhenReady() {
            synchronized (this) {
                if (provider==null || !isDone() || pendingUpload!=null || !uploadClosed.compareAndSet(false,true)) return;
            }
            try { uploads.execute(() -> { try { provider.close(); } catch (IOException ignored) {} }); }
            catch (RuntimeException ignored) {}
        }
        private void upload(HttpURLConnection opened) throws Exception {
            CompletableFuture<Long> lengthResult=uploadFuture();
            submitUpload(lengthResult,() -> lengthResult.complete(provider.getLength()));
            long length=await(lengthResult);
            if (length < -1) throw new IllegalArgumentException("Invalid upload length");
            opened.setDoOutput(true);
            if (length>=0) opened.setFixedLengthStreamingMode(length); else opened.setChunkedStreamingMode(32768);
            long sent=0;
            ByteBuffer buffer=ByteBuffer.allocateDirect((int)(length<0 ? 32768 : Math.min(32768,length)));
            byte[] bytes=new byte[buffer.capacity()];
            try (OutputStream output=opened.getOutputStream()) {
                while (!isDone() && (length<0 || sent<length)) {
                    buffer.clear();
                    if (length>=0) buffer.limit((int)Math.min(buffer.capacity(),length-sent));
                    CompletableFuture<Boolean> result=uploadFuture();
                    int limit=buffer.limit();
                    UploadDataSink sink=new UploadDataSink() {
                        @Override public void onReadSucceeded(boolean last) { result.complete(last); }
                        @Override public void onReadError(Exception e) { result.completeExceptionally(e); }
                        @Override public void onRewindSucceeded() { result.completeExceptionally(new IOException("Unexpected rewind completion")); }
                        @Override public void onRewindError(Exception e) { result.completeExceptionally(e); }
                    };
                    submitUpload(result,() -> provider.read(sink,buffer));
                    boolean last=await(result);
                    if (buffer.limit()!=limit || (length>=0 && last)) throw new IOException("Invalid upload completion");
                    int count=buffer.position();
                    if (count==0 && !last) throw new IOException("Upload provider made no progress");
                    buffer.flip(); buffer.get(bytes,0,count); output.write(bytes,0,count); sent+=count;
                    if (last) break;
                }
            }
        }
        private void rewind() throws Exception {
            CompletableFuture<Void> result=uploadFuture();
            UploadDataSink sink=new UploadDataSink() {
                @Override public void onReadSucceeded(boolean last) { result.completeExceptionally(new IOException("Unexpected read completion")); }
                @Override public void onReadError(Exception e) { result.completeExceptionally(e); }
                @Override public void onRewindSucceeded() { result.complete(null); }
                @Override public void onRewindError(Exception e) { result.completeExceptionally(e); }
            };
            submitUpload(result,() -> provider.rewind(sink));
            await(result);
        }
        @Override public void cancel() { if (phase!=NEW) finish(null,true); }
        private void fail(Exception e) {
            HttpException error;
            if (e instanceof HttpException) error=(HttpException)e;
            else error=new NetworkException("HTTP request failed",e) {
                @Override public int getErrorCode() {
                    return e instanceof UnknownHostException ? ERROR_HOSTNAME_NOT_RESOLVED
                            : e instanceof SocketTimeoutException ? ERROR_TIMED_OUT
                            : e instanceof ConnectException ? ERROR_CONNECTION_REFUSED : ERROR_OTHER;
                }
                @Override public boolean isImmediatelyRetryable() { return e instanceof SocketTimeoutException; }
            };
            finish(error,false);
        }
        private void finish(HttpException error, boolean canceled) {
            synchronized (this) {
                if (phase==DONE) return;
                phase=DONE; terminalError=error; terminalCanceled=canceled;
            }
            admission.cancelQueued();
            uploadCanceled.completeExceptionally(new CancellationException());
            closeUploadWhenReady();
            queueCleanup();
        }
    }
}
