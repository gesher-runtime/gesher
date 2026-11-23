// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.net.HttpURLConnection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.ArrayList;
import java.util.Map;
import java.util.Set;

/** Engine-owned storage; HTTP policy remains the pinned Android OkHttp implementation. */
final class EngineHttpCache implements AutoCloseable {
    private static final long DEFAULT_SIZE = 32L * 1024 * 1024;
    private static final Set<String> STORAGE_OWNERS = new HashSet<>();
    private final Api api;
    private final Object cache;
    private final Object adapter;
    private final MemoryFiles memory;
    private final String storage;
    private boolean closed;

    EngineHttpCache(int mode, long requestedSize, String path) {
        boolean enabled = mode == HttpEngine.Builder.HTTP_CACHE_IN_MEMORY
                || mode == HttpEngine.Builder.HTTP_CACHE_DISK;
        String owned = null;
        Api resolved = null;
        Object created = null;
        Object attached = null;
        MemoryFiles files = null;
        try {
            if (mode == HttpEngine.Builder.HTTP_CACHE_DISK
                    || mode == HttpEngine.Builder.HTTP_CACHE_DISK_NO_HTTP) {
                File directory = new File(path).getCanonicalFile();
                if (!directory.isDirectory()) throw new IllegalArgumentException("Storage path must be an existing directory");
                String candidate = directory.getPath();
                synchronized (STORAGE_OWNERS) {
                    if (!STORAGE_OWNERS.add(candidate)) throw new IllegalStateException("Storage path is already in use");
                    owned = candidate;
                }
            }
            if (enabled) {
                resolved = Api.get();
                long maximum = requestedSize == 0 ? DEFAULT_SIZE : requestedSize;
                if (mode == HttpEngine.Builder.HTTP_CACHE_IN_MEMORY) {
                    files = new MemoryFiles(resolved, maximum);
                    created = resolved.memoryCache.newInstance(new File("gesher-memory-cache"), maximum, files.proxy());
                } else {
                    // OkHttp may clear its whole directory after a corrupt journal.
                    // Never give it the application's storage directory itself.
                    created = resolved.diskCache.newInstance(new File(owned, "gesher-http-cache-v1"), maximum);
                }
                call(resolved.initialize, created);
                attached = resolved.privateAdapter(created);
            }
        } catch (Exception failure) {
            if (created != null) try { call(resolved.close, created); } catch (Exception ignored) {}
            if (files != null) files.close();
            if (owned != null) synchronized (STORAGE_OWNERS) { STORAGE_OWNERS.remove(owned); }
            if (failure instanceof RuntimeException) throw (RuntimeException) failure;
            throw new IllegalStateException("Cannot initialize the private HTTP cache", cause(failure));
        }
        api = resolved; cache = created; adapter = attached; memory = files; storage = owned;
    }

    void configure(HttpURLConnection connection, boolean bypass) throws IOException {
        try {
            String type = connection.getClass().getName();
            if (type.equals("com.android.okhttp.internal.huc.HttpURLConnectionImpl")
                    || type.equals("com.android.okhttp.internal.huc.HttpsURLConnectionImpl")) {
                Api provider = api == null ? Api.get() : api;
                Object http = provider.httpType.isInstance(connection) ? connection : provider.httpsDelegate.get(connection);
                Object client = provider.httpClient.get(http);
                // Clear the inherited global adapter even when private caching is
                // disabled: a URLConnection caller may later setUseCaches(true).
                // HUC already owns a copied client. In this AOSP revision,
                // public setCache is activated only by a subsequent client copy;
                // use the same InternalCache bridge as Android's handler here.
                call(provider.setInternalCache, provider.internal, client,
                        adapter);
            } else if (cache != null) {
                throw new IOException("Private HTTP caching requires the Android provider");
            }
            connection.setUseCaches(cache != null && !bypass);
        } catch (ReflectiveOperationException | IllegalArgumentException error) {
            throw new IOException("Cannot attach the private HTTP cache", cause(error));
        }
    }

    /** Inspect the provider's actual cached-body decision, including revalidation. */
    static boolean wasCached(HttpURLConnection connection) throws IOException {
        try {
            Api provider = Api.get();
            Object http = provider.httpType.isInstance(connection) ? connection : provider.httpsDelegate.get(connection);
            Object engine = provider.httpEngine.get(http);
            Object response = call(provider.response, engine);
            Object cached = call(provider.cachedResponse, response);
            if (cached == null) return false;
            Object network = call(provider.networkResponse, response);
            // HUC labels even a changed 200 as CONDITIONAL_CACHE. Reuse the
            // pinned provider's decision (304 or an older Last-Modified),
            // rather than treating that header or a server field as a hit.
            return network == null || (Boolean) call(provider.validate, null, cached, network);
        } catch (ReflectiveOperationException | IllegalArgumentException error) {
            throw new IOException("Cannot inspect the private HTTP cache response", cause(error));
        }
    }

    @Override public synchronized void close() {
        if (closed) return;
        closed = true;
        try {
            if (cache != null) call(api.close, cache);
        } catch (Exception ignored) {
            // A failed optional cache flush must not keep the engine or its
            // storage path alive. Responses never depend on successful caching.
        } finally {
            if (memory != null) memory.close();
            if (storage != null) synchronized (STORAGE_OWNERS) { STORAGE_OWNERS.remove(storage); }
        }
    }

    private static Throwable cause(Throwable error) {
        return error instanceof InvocationTargetException ? ((InvocationTargetException) error).getCause() : error;
    }

    private static Object call(Method method, Object receiver, Object... arguments) throws ReflectiveOperationException, IOException {
        try { return method.invoke(receiver, arguments); }
        catch (InvocationTargetException error) {
            Throwable cause = error.getCause();
            if (cause instanceof IOException) throw (IOException) cause;
            if (cause instanceof RuntimeException) throw (RuntimeException) cause;
            if (cause instanceof Error) throw (Error) cause;
            throw error;
        }
    }

    /** Reflection keeps the module API build independent of hidden provider classes. */
    private static final class Api {
        private static final String PROTOCOL_HEADER = "X-Gesher-Cache-Protocol";
        private static final String PROTOCOL_VERSION = "gesher-v1 ";
        final Class<?> httpType, fileSystem, source, sink, buffer, internalCache;
        final Field httpClient, httpEngine, httpsDelegate, cacheAdapter;
        final Constructor<?> diskCache, memoryCache;
        final Method initialize, close, setInternalCache, writeBytes, readBytes;
        final Method response, cachedResponse, networkResponse, validate;
        final Constructor<?> headersBuilder;
        final Method responseHeaders, responseProtocol, responseBuilder, responseBody, bodyClose;
        final Method headerCount, headerName, headerValue, headerAdd, headersBuild;
        final Method builderAddHeader, builderHeaders, builderProtocol, builderBuild, protocolGet;
        final Object timeout, internal;
        private static Api instance;
        static synchronized Api get() throws ReflectiveOperationException {
            if (instance == null) instance = new Api();
            return instance;
        }
        private Api() throws ReflectiveOperationException {
            Class<?> cache = Class.forName("com.android.okhttp.Cache");
            Class<?> client = Class.forName("com.android.okhttp.OkHttpClient");
            httpType = Class.forName("com.android.okhttp.internal.huc.HttpURLConnectionImpl");
            httpClient = httpType.getDeclaredField("client"); httpClient.setAccessible(true);
            httpEngine = httpType.getDeclaredField("httpEngine"); httpEngine.setAccessible(true);
            httpsDelegate = Class.forName("com.android.okhttp.internal.huc.HttpsURLConnectionImpl").getDeclaredField("delegate");
            httpsDelegate.setAccessible(true);
            fileSystem = Class.forName("com.android.okhttp.internal.io.FileSystem");
            source = Class.forName("com.android.okhttp.okio.Source");
            sink = Class.forName("com.android.okhttp.okio.Sink");
            buffer = Class.forName("com.android.okhttp.okio.Buffer");
            diskCache = cache.getConstructor(File.class, long.class);
            memoryCache = cache.getDeclaredConstructor(File.class, long.class, fileSystem);
            memoryCache.setAccessible(true);
            initialize = cache.getMethod("initialize"); close = cache.getMethod("close");
            Class<?> internalType = Class.forName("com.android.okhttp.internal.Internal");
            internal = internalType.getField("instance").get(null);
            setInternalCache = internalType.getMethod("setCache", client,
                    internalCache = Class.forName("com.android.okhttp.internal.InternalCache"));
            cacheAdapter = cache.getField("internalCache");
            writeBytes = buffer.getMethod("write", byte[].class, int.class, int.class);
            readBytes = buffer.getMethod("read", byte[].class, int.class, int.class);
            timeout = Class.forName("com.android.okhttp.okio.Timeout").getField("NONE").get(null);
            Class<?> engineType = Class.forName("com.android.okhttp.internal.http.HttpEngine");
            Class<?> responseType = Class.forName("com.android.okhttp.Response");
            response = engineType.getMethod("getResponse");
            cachedResponse = responseType.getMethod("cacheResponse");
            networkResponse = responseType.getMethod("networkResponse");
            validate = engineType.getDeclaredMethod("validate", responseType, responseType);
            validate.setAccessible(true);
            Class<?> headersType = Class.forName("com.android.okhttp.Headers");
            Class<?> headersBuilderType = Class.forName("com.android.okhttp.Headers$Builder");
            Class<?> responseBuilderType = Class.forName("com.android.okhttp.Response$Builder");
            Class<?> protocolType = Class.forName("com.android.okhttp.Protocol");
            responseHeaders = responseType.getMethod("headers");
            responseProtocol = responseType.getMethod("protocol");
            responseBuilder = responseType.getMethod("newBuilder");
            responseBody = responseType.getMethod("body");
            bodyClose = Class.forName("com.android.okhttp.ResponseBody").getMethod("close");
            headersBuilder = headersBuilderType.getConstructor();
            headerCount = headersType.getMethod("size");
            headerName = headersType.getMethod("name", int.class);
            headerValue = headersType.getMethod("value", int.class);
            // Preserve exactly what the provider's network/cache parser accepts;
            // public Builder.add would newly reject its lenient response fields.
            headerAdd = headersBuilderType.getDeclaredMethod("addLenient", String.class, String.class);
            headerAdd.setAccessible(true);
            headersBuild = headersBuilderType.getMethod("build");
            builderAddHeader = responseBuilderType.getMethod("addHeader", String.class, String.class);
            builderHeaders = responseBuilderType.getMethod("headers", headersType);
            builderProtocol = responseBuilderType.getMethod("protocol", protocolType);
            builderBuild = responseBuilderType.getMethod("build");
            protocolGet = protocolType.getMethod("get", String.class);
        }
        Object privateAdapter(Object cache) throws ReflectiveOperationException {
            Object delegate = cacheAdapter.get(cache);
            return Proxy.newProxyInstance(internalCache.getClassLoader(), new Class<?>[]{internalCache},
                    (proxy, method, args) -> {
                if (method.getDeclaringClass() == Object.class) {
                    if (method.getName().equals("equals")) return proxy == args[0];
                    if (method.getName().equals("hashCode")) return System.identityHashCode(proxy);
                    return "EngineHttpCache.InternalCache";
                }
                switch (method.getName()) {
                    case "get": {
                        Object found = call(method, delegate, args);
                        if (found == null) return null;
                        Object restored = restoreProtocol(found);
                        if (restored == null) {
                            // A damaged/old metadata record is only a cache miss.
                            Object body = call(responseBody, found);
                            if (body != null) try { call(bodyClose, body); } catch (IOException ignored) {}
                        }
                        return restored;
                    }
                    case "put": return call(method, delegate, storeProtocol(args[0]));
                    case "update": return call(method, delegate, args[0], storeProtocol(args[1]));
                    default: return call(method, delegate, args);
                }
            });
        }
        private Object storeProtocol(Object response) throws ReflectiveOperationException, IOException {
            Object builder = call(responseBuilder, response);
            // Only the copy handed to Cache sees this final field. The original
            // network response, including identically named server fields, stays
            // untouched. StatusLine alone loses h2 when OkHttp persists a cache.
            call(builderAddHeader, builder, PROTOCOL_HEADER,
                    PROTOCOL_VERSION + call(responseProtocol, response));
            return call(builderBuild, builder);
        }
        private Object restoreProtocol(Object response) throws ReflectiveOperationException, IOException {
            Object headers = call(responseHeaders, response);
            int count = (Integer) call(headerCount, headers), metadata = -1;
            for (int i = 0; i < count; i++)
                if (PROTOCOL_HEADER.equalsIgnoreCase((String) call(headerName, headers, i))) metadata = i;
            if (metadata == -1) return null;
            String value = (String) call(headerValue, headers, metadata);
            if (!value.startsWith(PROTOCOL_VERSION)) return null;
            Object protocol;
            try { protocol = call(protocolGet, null, value.substring(PROTOCOL_VERSION.length())); }
            catch (IOException invalid) { return null; }
            Object kept = headersBuilder.newInstance();
            for (int i = 0; i < count; i++) if (i != metadata)
                call(headerAdd, kept, call(headerName, headers, i), call(headerValue, headers, i));
            Object builder = call(responseBuilder, response);
            call(builderHeaders, builder, call(headersBuild, kept));
            call(builderProtocol, builder, protocol);
            return call(builderBuild, builder);
        }
    }

    /**
     * DiskLruCache over memory: no temporary files and no second HTTP policy.
     * Its size is advisory; cap in-progress writes and retained reader snapshots
     * too. Journal headroom permits an oversized body to abort without consuming
     * unbounded memory. Cache write errors are handled by OkHttp's FaultHidingSink.
     */
    private static final class MemoryFiles implements InvocationHandler {
        final Api api;
        final long limit;
        final Map<String, Node> files = new HashMap<>();
        long held;
        boolean closed;
        static final int CHUNK = 8192;
        final class Node {
            final ArrayList<byte[]> chunks = new ArrayList<>();
            long size, capacity;
            int readers, writers;
            boolean linked = true;
        }
        MemoryFiles(Api api, long maximum) {
            this.api = api;
            long slack = 1024L * 1024;
            limit = maximum > (Long.MAX_VALUE - slack) / 2 ? Long.MAX_VALUE : maximum * 2 + slack;
        }
        Object proxy() { return proxy(api.fileSystem, this); }
        private Object proxy(Class<?> type, InvocationHandler handler) {
            return Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[]{type}, handler);
        }
        private Object objectMethod(Object proxy, Method method, Object[] args) {
            if (method.getName().equals("toString")) return "EngineHttpCache." + proxy.getClass().getInterfaces()[0].getSimpleName();
            if (method.getName().equals("hashCode")) return System.identityHashCode(proxy);
            if (method.getName().equals("equals")) return proxy == args[0];
            throw new UnsupportedOperationException(method.toString());
        }
        private void retire(Node node) {
            if (!node.linked && node.readers == 0 && node.writers == 0) {
                held -= node.capacity; node.capacity = node.size = 0; node.chunks.clear();
            }
        }
        private void unlink(String key) throws Exception {
            Node node = files.remove(key);
            if (node != null) { node.linked = false; retire(node); }
        }
        @Override public synchronized Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            if (method.getDeclaringClass() == Object.class) return objectMethod(proxy, method, args);
            if (closed) throw new IOException("Memory cache is closed");
            String key = ((File) args[0]).getPath();
            Node node = files.get(key);
            switch (method.getName()) {
                case "exists": return node != null;
                case "size": return node == null ? 0L : node.size;
                case "delete": unlink(key); return null;
                case "deleteContents":
                    for (String name : files.keySet().toArray(new String[0])) {
                        if (name.startsWith(key + File.separator)) unlink(name);
                    }
                    return null;
                case "rename":
                    if (node == null) throw new FileNotFoundException(key);
                    String destination = ((File) args[1]).getPath();
                    if (!destination.equals(key)) { unlink(destination); files.remove(key); files.put(destination, node); }
                    return null;
                case "source":
                    if (node == null) throw new FileNotFoundException(key);
                    node.readers++;
                    return source(node, node.size);
                case "sink": unlink(key); node = null; // Fall through to new writable file.
                case "appendingSink":
                    if (node == null) { node = new Node(); files.put(key, node); }
                    node.writers++;
                    return sink(node);
                default: throw new UnsupportedOperationException(method.toString());
            }
        }
        private Object source(Node node, long length) {
            return proxy(api.source, new InvocationHandler() {
                boolean done;
                long position;
                @Override public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
                    synchronized (MemoryFiles.this) {
                        if (method.getDeclaringClass() == Object.class) return objectMethod(proxy, method, args);
                        switch (method.getName()) {
                            case "timeout": return api.timeout;
                            case "close":
                                if (!done) { done = true; node.readers--; retire(node); }
                                return null;
                            case "read":
                                if (done) throw new IOException("Cache source is closed");
                                long count = (Long) args[1];
                                if (count < 0) throw new IllegalArgumentException("Negative byte count");
                                if (count == 0) return 0L;
                                if (position == length) return -1L;
                                int offset = (int) (position % CHUNK);
                                int read = (int) Math.min(Math.min(count, length - position), CHUNK - offset);
                                call(api.writeBytes, args[0], node.chunks.get((int) (position / CHUNK)), offset, read);
                                position += read;
                                return (long) read;
                            default: throw new UnsupportedOperationException(method.toString());
                        }
                    }
                }
            });
        }
        private Object sink(Node node) {
            return proxy(api.sink, new InvocationHandler() {
                boolean done;
                @Override public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
                    synchronized (MemoryFiles.this) {
                        if (method.getDeclaringClass() == Object.class) return objectMethod(proxy, method, args);
                        switch (method.getName()) {
                            case "timeout": return api.timeout;
                            case "close":
                                if (!done) { done = true; node.writers--; retire(node); }
                                return null;
                            case "flush":
                                if (done) throw new IOException("Cache sink is closed");
                                return null;
                            case "write":
                                if (done || closed) throw new IOException("Cache sink is closed");
                                long count = (Long) args[1];
                                long available = node.capacity - node.size;
                                if (count < 0 || count > available && count - available > limit - held)
                                    throw new IOException("Memory cache capacity exceeded");
                                long added = count <= available ? 0 : count - available;
                                // Charge allocated array capacity, including the final partial
                                // chunk. Shared Okio segments could otherwise amplify tiny
                                // network reads into one retained 8KiB array per byte.
                                if (added > 0 && (added - 1) / CHUNK + 1 > (limit - held) / CHUNK)
                                    throw new IOException("Memory cache capacity exceeded");
                                while (count > 0) {
                                    int offset = (int) (node.size % CHUNK);
                                    if (node.size == node.capacity) {
                                        node.chunks.add(new byte[CHUNK]); node.capacity += CHUNK; held += CHUNK;
                                    }
                                    int step = (int) Math.min(count, CHUNK - offset);
                                    int read = (Integer) call(api.readBytes, args[0], node.chunks.get((int) (node.size / CHUNK)), offset, step);
                                    if (read <= 0) throw new IOException("Truncated cache write buffer");
                                    node.size += read; count -= read;
                                }
                                return null;
                            default: throw new UnsupportedOperationException(method.toString());
                        }
                    }
                }
            });
        }
        synchronized void close() {
            closed = true;
            for (String key : files.keySet().toArray(new String[0])) {
                try { unlink(key); } catch (Exception ignored) {}
            }
        }
    }
}
