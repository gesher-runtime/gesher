// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.ProtocolException;
import java.net.URL;
import java.security.Permission;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/** Keeps the provider's private protocol controls out of HttpEngine's header API. */
final class JavaEngineConnection extends HttpURLConnection {
    private final JavaHttpEngine engine;
    private final HttpURLConnection delegate;
    // Guarded by this. Never hold the monitor while calling the transport.
    private boolean started, registered, finished, disconnected;
    private int operations;
    private int responseCode = -1;
    private TrackedInput input;

    JavaEngineConnection(JavaHttpEngine engine, HttpURLConnection delegate) {
        super(delegate.getURL());
        this.engine = engine;
        this.delegate = delegate;
    }

    private synchronized void enter() {
        if (!started || finished && !registered && responseCode == -1 && !disconnected) {
            engine.configureContentEncoding(delegate);
            engine.register(this); // Also rejects a previously opened, unused connection after shutdown.
            started = true;
            registered = true;
            finished = false;
        }
        operations++;
    }
    private synchronized void leave() { operations--; releaseIfFinished(); }
    private synchronized void finish() { finished = true; releaseIfFinished(); }
    private void releaseIfFinished() {
        if (registered && finished && operations == 0) {
            registered = false;
            engine.release(this);
        }
    }
    private void failed() {
        try {
            try { delegate.disconnect(); } catch (RuntimeException ignored) {}
            TrackedInput stream;
            synchronized (this) { stream = input; }
            if (stream != null) try { stream.close(); } catch (IOException | RuntimeException ignored) {}
        } finally { finish(); }
    }
    @FunctionalInterface private interface Io<T> { T run() throws IOException; }
    private <T> T call(Io<T> operation) throws IOException {
        return call(operation, false);
    }
    private <T> T call(Io<T> operation, boolean localProtocolError) throws IOException {
        enter();
        try {
            T value = operation.run();
            synchronized (this) {
                // disconnect can race provider initialization, before its own
                // cancel handle exists. Retire that newly created transport too.
                if (disconnected && responseCode == -1) throw new IOException("Connection was disconnected");
            }
            return value;
        }
        catch (ProtocolException error) { if (!localProtocolError) failed(); throw error; }
        catch (IOException | RuntimeException | Error error) { failed(); throw error; }
        finally { leave(); }
    }

    /** Obtain headers once and retain the response body even when only a header was requested. */
    private int response() throws IOException {
        synchronized (this) {
            if (responseCode != -1) return responseCode;
            if (disconnected) throw new IOException("Connection was disconnected");
        }
        int code = delegate.getResponseCode();
        InputStream body = code >= 400 ? delegate.getErrorStream()
                : delegate.getDoInput() ? delegate.getInputStream() : null;
        try { body = engine.decodeBody(body, delegate); }
        catch (IOException | RuntimeException | Error failure) {
            if (body != null) try { body.close(); }
            catch (IOException cleanup) { failure.addSuppressed(cleanup); }
            throw failure;
        }
        TrackedInput tracked = body == null ? null : new TrackedInput(body);
        boolean cancel;
        synchronized (this) {
            responseCode = code;
            if (input == null) input = tracked;
            else tracked = input;
            cancel = disconnected;
        }
        if (cancel) {
            if (tracked != null) tracked.close();
            throw new IOException("Connection was disconnected");
        }
        long length = delegate.getHeaderFieldLong("Content-Length", -1);
        boolean chunked = "chunked".equalsIgnoreCase(delegate.getHeaderField("Transfer-Encoding"));
        boolean empty = "HEAD".equals(delegate.getRequestMethod()) || !chunked && (length == 0
                || length == -1 && (code >= 100 && code < 200 || code == 204 || code == 304));
        if (empty) {
            if (tracked != null) tracked.eof();
            else finish();
        }
        return code;
    }
    private <T> T headers(Io<T> operation, T failure) {
        try { return call(() -> { response(); return operation.run(); }); }
        catch (IOException error) { return failure; }
    }

    private final class TrackedInput extends InputStream {
        private final InputStream source;
        private volatile boolean closed, ended;
        TrackedInput(InputStream source) { this.source = source; }
        void eof() throws IOException {
            ended = true;
            close(); // A cache snapshot/editor is still owned until its stream closes.
        }
        @Override public int read() throws IOException {
            if (ended) return -1;
            if (closed) throw new IOException("Stream is closed");
            return call(() -> {
                int value = source.read();
                if (value == -1) eof();
                return value;
            });
        }
        @Override public int read(byte[] bytes, int offset, int length) throws IOException {
            Objects.checkFromIndexSize(offset, length, bytes.length);
            if (length == 0) return 0;
            if (ended) return -1;
            if (closed) throw new IOException("Stream is closed");
            return call(() -> {
                int count = source.read(bytes, offset, length);
                if (count == -1) eof();
                return count;
            });
        }
        @Override public int available() throws IOException {
            if (ended) return 0;
            if (closed) throw new IOException("Stream is closed");
            return call(source::available);
        }
        @Override public void close() throws IOException {
            synchronized (this) { if (closed) return; closed = true; }
            enter();
            try { source.close(); }
            finally { finish(); leave(); }
        }
    }

    @Override public void setRequestProperty(String name, String value) {
        delegate.setRequestProperty(JavaHttpEngine.transportHeaderName(name), value);
    }
    @Override public void addRequestProperty(String name, String value) {
        delegate.addRequestProperty(JavaHttpEngine.transportHeaderName(name), value);
    }
    @Override public String getRequestProperty(String name) { return delegate.getRequestProperty(name); }
    @Override public Map<String, List<String>> getRequestProperties() { return delegate.getRequestProperties(); }
    @Override public void connect() throws IOException { call(() -> { delegate.connect(); return null; }); }
    @Override public void disconnect() {
        synchronized (this) {
            if (!started) { delegate.disconnect(); return; }
            disconnected = true;
            operations++;
        }
        try { failed(); }
        finally { leave(); }
    }
    @Override public boolean usingProxy() { return delegate.usingProxy(); }
    @Override public URL getURL() { return delegate.getURL(); }
    @Override public InputStream getInputStream() throws IOException {
        // Preserve the provider's local validation without starting a request.
        if (!delegate.getDoInput()) return delegate.getInputStream();
        int code = call(this::response);
        // An HTTP error has a live error body, not a failed transport. Keep its owner.
        if (code >= 400) return delegate.getInputStream();
        synchronized (this) { return input; }
    }
    @Override public OutputStream getOutputStream() throws IOException {
        connect();
        // The provider has connected successfully. Asking to upload on a GET or
        // after response headers is a local error; the response can still be read.
        OutputStream output = call(delegate::getOutputStream, true);
        return new OutputStream() {
            @Override public void write(int value) throws IOException {
                call(() -> { output.write(value); return null; });
            }
            @Override public void write(byte[] bytes, int offset, int length) throws IOException {
                Objects.checkFromIndexSize(offset, length, bytes.length);
                call(() -> { output.write(bytes, offset, length); return null; });
            }
            @Override public void flush() throws IOException { call(() -> { output.flush(); return null; }); }
            @Override public void close() throws IOException { call(() -> { output.close(); return null; }); }
        };
    }
    @Override public InputStream getErrorStream() {
        return headers(() -> { synchronized (this) { return responseCode >= 400 ? input : null; } }, null);
    }
    // Content handlers must receive this connection so returned streams retain ownership.
    @Override public Object getContent() throws IOException { return super.getContent(); }
    @Override public Object getContent(Class[] types) throws IOException { return super.getContent(types); }
    @Override public Permission getPermission() throws IOException { return delegate.getPermission(); }
    @Override public int getResponseCode() throws IOException { return call(this::response); }
    @Override public String getResponseMessage() throws IOException {
        return call(() -> { response(); return delegate.getResponseMessage(); });
    }
    @Override public String getHeaderField(String name) { return headers(() -> delegate.getHeaderField(name), null); }
    @Override public String getHeaderField(int index) { return headers(() -> delegate.getHeaderField(index), null); }
    @Override public String getHeaderFieldKey(int index) { return headers(() -> delegate.getHeaderFieldKey(index), null); }
    @Override public Map<String, List<String>> getHeaderFields() {
        return headers(delegate::getHeaderFields, Collections.emptyMap());
    }
    @Override public void setRequestMethod(String method) throws ProtocolException { delegate.setRequestMethod(method); }
    @Override public String getRequestMethod() { return delegate.getRequestMethod(); }
    @Override public void setFixedLengthStreamingMode(int length) { delegate.setFixedLengthStreamingMode(length); }
    @Override public void setFixedLengthStreamingMode(long length) { delegate.setFixedLengthStreamingMode(length); }
    @Override public void setChunkedStreamingMode(int length) { delegate.setChunkedStreamingMode(length); }
    @Override public void setInstanceFollowRedirects(boolean follow) { delegate.setInstanceFollowRedirects(follow); }
    @Override public boolean getInstanceFollowRedirects() { return delegate.getInstanceFollowRedirects(); }
    @Override public void setConnectTimeout(int timeout) { delegate.setConnectTimeout(timeout); }
    @Override public int getConnectTimeout() { return delegate.getConnectTimeout(); }
    @Override public void setReadTimeout(int timeout) { delegate.setReadTimeout(timeout); }
    @Override public int getReadTimeout() { return delegate.getReadTimeout(); }
    @Override public void setDoInput(boolean value) { delegate.setDoInput(value); }
    @Override public boolean getDoInput() { return delegate.getDoInput(); }
    @Override public void setDoOutput(boolean value) { delegate.setDoOutput(value); }
    @Override public boolean getDoOutput() { return delegate.getDoOutput(); }
    @Override public void setAllowUserInteraction(boolean value) { delegate.setAllowUserInteraction(value); }
    @Override public boolean getAllowUserInteraction() { return delegate.getAllowUserInteraction(); }
    @Override public void setUseCaches(boolean value) { delegate.setUseCaches(value); }
    @Override public boolean getUseCaches() { return delegate.getUseCaches(); }
    @Override public void setDefaultUseCaches(boolean value) { delegate.setDefaultUseCaches(value); }
    @Override public boolean getDefaultUseCaches() { return delegate.getDefaultUseCaches(); }
    @Override public void setIfModifiedSince(long value) { delegate.setIfModifiedSince(value); }
    @Override public long getIfModifiedSince() { return delegate.getIfModifiedSince(); }
}
