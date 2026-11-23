// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import java.io.IOException;
import java.io.InputStream;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.zip.GZIPInputStream;
import java.util.zip.InflaterInputStream;

/** HTTP content coding policy shared by unary, URLConnection and duplex paths. */
final class ContentDecoding {
    private ContentDecoding() {}

    static String acceptEncoding(URL url, boolean range) {
        if (range) return "identity";
        return url.getProtocol().equalsIgnoreCase("https") || isLocalhost(url.getHost())
                ? "gzip, deflate, br" : "gzip, deflate";
    }
    private static boolean isLocalhost(String host) {
        String value = host.toLowerCase(Locale.ROOT);
        if (value.endsWith(".")) value = value.substring(0, value.length() - 1);
        if (value.equals("localhost") || value.endsWith(".localhost")) return true;
        if (value.startsWith("[") && value.endsWith("]")) value = value.substring(1, value.length() - 1);
        if (value.equals("::1") || value.equals("0:0:0:0:0:0:0:1")) return true;
        String[] parts = value.split("\\.", -1);
        if (parts.length != 4 || !parts[0].equals("127")) return false;
        for (String part : parts) {
            if (part.isEmpty() || part.length() > 3) return false;
            int number = 0;
            for (int i = 0; i < part.length(); i++) {
                char c = part.charAt(i);
                if (c < '0' || c > '9') return false;
                number = number * 10 + c - '0';
            }
            if (number > 255) return false;
        }
        return true;
    }

    static String encoding(List<Map.Entry<String, String>> headers) {
        StringBuilder result = new StringBuilder();
        for (Map.Entry<String, String> header : headers) {
            if (!header.getKey().equalsIgnoreCase("Content-Encoding")) continue;
            if (result.length() != 0) result.append(',');
            result.append(header.getValue());
        }
        return result.toString();
    }

    static InputStream wrap(InputStream source, String encoding, boolean brotli) throws IOException {
        if (source == null || encoding == null || encoding.isEmpty()) return source;
        List<String> types = new ArrayList<>();
        for (String token : encoding.split(",", -1)) {
            String type = token.trim().toLowerCase(Locale.ROOT);
            // As Chromium does, unknown/identity coding leaves the whole body
            // untouched. Never partially decode an unrecognized coding chain.
            if (!(type.equals("gzip") || type.equals("x-gzip") || type.equals("deflate")
                    || brotli && type.equals("br"))) return source;
            types.add(type);
        }
        if (types.size() > 10) throw new IOException("Too many nested HTTP content encodings");
        return new Deferred(source, types);
    }

    // Preserve network-body accounting when a decoded read returns many more
    // bytes than arrived on the wire. Header-byte accounting remains the
    // underlying transport's separate responsibility.
    static long encodedBytes(InputStream source) {
        return source instanceof Deferred ? ((Deferred) source).source.received : -1;
    }

    private static final class Counted extends InputStream {
        final InputStream source;
        volatile long received;
        private boolean closed;
        Counted(InputStream source) { this.source = source; }
        @Override public int read() throws IOException {
            int value = source.read();
            if (value >= 0) received++;
            return value;
        }
        @Override public int read(byte[] bytes, int offset, int count) throws IOException {
            int value = source.read(bytes, offset, count);
            if (value > 0) received += value;
            return value;
        }
        @Override public void close() throws IOException {
            synchronized (this) { if (closed) return; closed=true; }
            source.close();
        }
    }

    private static final class Deferred extends InputStream {
        final Counted source;
        final List<String> types;
        volatile InputStream decoded;
        volatile boolean closed;
        Deferred(InputStream source, List<String> types) {
            this.source = new Counted(source); this.types = types;
        }
        private InputStream body() throws IOException {
            if (closed) throw new IOException("Stream is closed");
            InputStream ready = decoded;
            if (ready == null) {
                ready = source;
                try {
                    for (int i = types.size() - 1; i >= 0; i--) {
                        String type = types.get(i);
                        ready = type.equals("br") ? new BrotliInputStream(ready)
                                : type.equals("deflate") ? new InflaterInputStream(ready)
                                : new GZIPInputStream(ready);
                    }
                    decoded = ready;
                    if (closed) throw new IOException("Stream is closed");
                } catch (IOException | RuntimeException | Error failure) {
                    try { ready.close(); } catch (IOException cleanup) { failure.addSuppressed(cleanup); }
                    throw failure;
                }
            }
            return ready;
        }
        @Override public int read() throws IOException { return body().read(); }
        @Override public int read(byte[] bytes, int offset, int count) throws IOException {
            Objects.checkFromIndexSize(offset, count, bytes.length);
            if (count == 0) return 0;
            return body().read(bytes, offset, count);
        }
        @Override public void close() throws IOException {
            if (closed) return;
            closed = true;
            // A decoder constructor can be waiting for input. Closing the raw
            // source first wakes it without taking a monitor held by that read.
            try { source.close(); }
            finally { InputStream ready = decoded; if (ready != null) ready.close(); }
        }
    }
}
