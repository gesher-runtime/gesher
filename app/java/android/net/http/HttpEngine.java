// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import android.content.Context;
import android.net.Network;
import java.io.IOException;
import java.net.URL;
import java.net.URLConnection;
import java.net.URLStreamHandlerFactory;
import java.time.Instant;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.Executor;

/** Android's asynchronous HTTP API, backed by Gesher's existing Java HTTP/TLS transport. */
public abstract class HttpEngine {
    public static String getVersionString() { return "1.0.0.0"; }
    public void bindToNetwork(Network network) { throw new UnsupportedOperationException("Network binding"); }
    public abstract URLStreamHandlerFactory createUrlStreamHandlerFactory();
    public abstract BidirectionalStream.Builder newBidirectionalStreamBuilder(String url, Executor executor, BidirectionalStream.Callback callback);
    public abstract UrlRequest.Builder newUrlRequestBuilder(String url, Executor executor, UrlRequest.Callback callback);
    public abstract URLConnection openConnection(URL url) throws IOException;
    public abstract void shutdown();

    public static class Builder {
        public static final int HTTP_CACHE_DISABLED=0, HTTP_CACHE_IN_MEMORY=1,
                HTTP_CACHE_DISK_NO_HTTP=2, HTTP_CACHE_DISK=3;
        private String agent;
        private boolean http2Enabled = true;
        private boolean brotliEnabled;
        private int cacheMode = HTTP_CACHE_DISABLED;
        private long cacheSize;
        private String storagePath;
        public Builder(Context context) { Objects.requireNonNull(context, "Context is required"); }
        public String getDefaultUserAgent() { return System.getProperty("http.agent", "Gesher/1.0"); }
        public Builder setUserAgent(String value) { agent=Objects.requireNonNull(value); return this; }
        public Builder setEnableBrotli(boolean enabled) { brotliEnabled = enabled; return this; }
        public Builder setEnableHttp2(boolean enabled) {
            http2Enabled = enabled;
            return this;
        }
        public Builder setEnableQuic(boolean enabled) { return this; }
        public Builder setEnablePublicKeyPinningBypassForLocalTrustAnchors(boolean enabled) { return this; }
        public Builder setEnableHttpCache(int mode, long size) {
            if (mode<0 || mode>3 || size<0) throw new IllegalArgumentException("Invalid HTTP cache configuration");
            validateCachePath(mode);
            cacheMode = mode; cacheSize = size;
            return this;
        }
        public Builder setStoragePath(String path) {
            if (!new java.io.File(Objects.requireNonNull(path)).isDirectory())
                throw new IllegalArgumentException("Storage path must be an existing directory");
            storagePath = path;
            return this;
        }
        private void validateCachePath(int mode) {
            boolean disk = mode == HTTP_CACHE_DISK || mode == HTTP_CACHE_DISK_NO_HTTP;
            if (disk && storagePath == null) throw new IllegalArgumentException("Storage path must be set");
        }
        public Builder addQuicHint(String host, int port, int alternatePort) {
            Objects.requireNonNull(host);
            if (port<1 || port>65535 || alternatePort<1 || alternatePort>65535)
                throw new IllegalArgumentException("Invalid port");
            return this;
        }
        public Builder addPublicKeyPins(String host, Set<byte[]> pins, boolean includeSubdomains, Instant expiration) {
            Objects.requireNonNull(host); Objects.requireNonNull(pins); Objects.requireNonNull(expiration);
            // Never silently drop an application's additional trust requirement.
            throw new UnsupportedOperationException("Public key pinning is not supported by this HttpEngine transport");
        }
        public Builder setConnectionMigrationOptions(ConnectionMigrationOptions options) { Objects.requireNonNull(options); return this; }
        public Builder setDnsOptions(DnsOptions options) { Objects.requireNonNull(options); return this; }
        public Builder setQuicOptions(QuicOptions options) { Objects.requireNonNull(options); return this; }
        public HttpEngine build() {
            validateCachePath(cacheMode);
            if (brotliEnabled) BrotliInputStream.ensureAvailable();
            return new JavaHttpEngine(agent == null ? getDefaultUserAgent() : agent, http2Enabled, brotliEnabled,
                    new EngineHttpCache(cacheMode, cacheSize, storagePath));
        }
    }
}
