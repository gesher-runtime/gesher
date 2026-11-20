// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.ProtocolException;
import java.net.URL;
import java.security.Permission;
import java.util.List;
import java.util.Map;

/** Keeps the provider's private protocol controls out of HttpEngine's header API. */
final class JavaEngineConnection extends HttpURLConnection {
    private final HttpURLConnection delegate;

    JavaEngineConnection(HttpURLConnection delegate) {
        super(delegate.getURL());
        this.delegate = delegate;
    }

    @Override public void setRequestProperty(String name, String value) {
        delegate.setRequestProperty(JavaHttpEngine.transportHeaderName(name), value);
    }
    @Override public void addRequestProperty(String name, String value) {
        delegate.addRequestProperty(JavaHttpEngine.transportHeaderName(name), value);
    }
    @Override public String getRequestProperty(String name) { return delegate.getRequestProperty(name); }
    @Override public Map<String, List<String>> getRequestProperties() { return delegate.getRequestProperties(); }
    @Override public void connect() throws IOException { delegate.connect(); }
    @Override public void disconnect() { delegate.disconnect(); }
    @Override public boolean usingProxy() { return delegate.usingProxy(); }
    @Override public URL getURL() { return delegate.getURL(); }
    @Override public InputStream getInputStream() throws IOException { return delegate.getInputStream(); }
    @Override public OutputStream getOutputStream() throws IOException { return delegate.getOutputStream(); }
    @Override public InputStream getErrorStream() { return delegate.getErrorStream(); }
    @Override public Object getContent() throws IOException { return delegate.getContent(); }
    @Override public Object getContent(Class[] types) throws IOException { return delegate.getContent(types); }
    @Override public Permission getPermission() throws IOException { return delegate.getPermission(); }
    @Override public int getResponseCode() throws IOException { return delegate.getResponseCode(); }
    @Override public String getResponseMessage() throws IOException { return delegate.getResponseMessage(); }
    @Override public String getHeaderField(String name) { return delegate.getHeaderField(name); }
    @Override public String getHeaderField(int index) { return delegate.getHeaderField(index); }
    @Override public String getHeaderFieldKey(int index) { return delegate.getHeaderFieldKey(index); }
    @Override public Map<String, List<String>> getHeaderFields() { return delegate.getHeaderFields(); }
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
