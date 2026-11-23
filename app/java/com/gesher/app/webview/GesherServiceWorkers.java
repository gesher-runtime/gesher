/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app.webview;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Process;
import android.webkit.ServiceWorkerClient;
import android.webkit.ServiceWorkerController;
import android.webkit.ServiceWorkerWebSettings;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** Prepared worker dispatch; unavailable until request failures preserve Android semantics. */
final class GesherServiceWorkers extends ServiceWorkerController {
    // No provider entry point publishes a controller while activation is unavailable.
    private static volatile GesherServiceWorkers sInstance;
    private final Context mContext;
    private final Settings mSettings;
    private volatile ServiceWorkerClient mClient;
    private final ThreadPoolExecutor mRequests = new ThreadPoolExecutor(2, 2, 15,
            TimeUnit.SECONDS, new ArrayBlockingQueue<>(64), task -> {
                Thread thread = new Thread(task, "WebView-service-worker");
                thread.setDaemon(true);
                return thread;
            });

    GesherServiceWorkers(Context context) {
        mContext = context;
        mSettings = new Settings(context.checkPermission(Manifest.permission.INTERNET,
                Process.myPid(), Process.myUid()) == PackageManager.PERMISSION_GRANTED);
        mRequests.allowCoreThreadTimeOut(true);
    }

    static GesherServiceWorkers get() {
        // Both framework and AndroidX controller access must fail before configuring
        // WebView or publishing a client. An HTTP 403 is not an Android network error.
        throw new UnsupportedOperationException(
                "WebView2 service-worker interception lacks Android request-failure support");
    }

    static boolean dispatch(long id, String url, String method, String headers) {
        GesherServiceWorkers controller = sInstance;
        if (controller == null) return false;
        try {
            controller.mRequests.execute(() -> controller.answer(id, url, method, headers));
            return true;
        } catch (RejectedExecutionException failure) {
            return false; // Native keeps the already-installed denied response.
        }
    }

    @Override public ServiceWorkerWebSettings getServiceWorkerWebSettings() { return mSettings; }
    @Override public void setServiceWorkerClient(ServiceWorkerClient client) { mClient = client; }

    private void answer(long id, String url, String method, String headers) {
        try {
            WebResourceResponse response = intercept(url, method, headers);
            if (response == null) {
                WebView2Host.respondToServiceWorker(id, 0, "", "", null);
                return;
            }
            try (InputStream input = response.getData()) {
                int status = response.getStatusCode() > 0 ? response.getStatusCode() : 200;
                String reason = response.getReasonPhrase() != null ? response.getReasonPhrase() : "OK";
                if (reason.indexOf('\r') >= 0 || reason.indexOf('\n') >= 0) {
                    throw new IllegalArgumentException("Invalid intercepted response reason");
                }
                StringBuilder output = new StringBuilder();
                String mime = response.getMimeType();
                if (mime != null) {
                    appendHeader(output, "Content-Type", mime + (response.getEncoding() == null
                            ? "" : "; charset=" + response.getEncoding()));
                }
                Map<String, String> extra = response.getResponseHeaders();
                if (extra != null) for (Map.Entry<String, String> entry : extra.entrySet()) {
                    if (mime == null || !"content-type".equalsIgnoreCase(entry.getKey())) {
                        appendHeader(output, entry.getKey(), entry.getValue());
                    }
                }
                WebView2Host.respondToServiceWorker(id, status, reason, output.toString(),
                        read(input));
            }
        } catch (RuntimeException | IOException failure) {
            // Do not release the original request on failed callback/stream/permission.
            WebView2Host.respondToServiceWorker(id, -1, "", "", null);
        }
    }

    WebResourceResponse intercept(String url, String method, String headers) {
        String appUrl = FileUrls.fromWebView2(url);
        ServiceWorkerClient client = mClient;
        WebResourceResponse response = client == null ? null
                : client.shouldInterceptRequest(new Request(appUrl, method, headers));
        if (response != null) return response;
        Uri uri = Uri.parse(appUrl);
        String scheme = uri.getScheme();
        if ("file".equalsIgnoreCase(scheme)) {
            return FileUrls.load(mContext, appUrl, mSettings.getAllowFileAccess());
        }
        if ("content".equalsIgnoreCase(scheme)) {
            if (!mSettings.getAllowContentAccess()) throw new SecurityException("Content access disabled");
            try {
                InputStream input = mContext.getContentResolver().openInputStream(uri);
                if (input == null) throw new IOException("Content is unavailable");
                return new WebResourceResponse(mContext.getContentResolver().getType(uri), null, input);
            } catch (IOException failure) {
                throw new IllegalStateException("Content is unavailable", failure);
            }
        }
        if (("http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme))
                && mSettings.getBlockNetworkLoads()) {
            throw new SecurityException("Service-worker network loads disabled");
        }
        return null;
    }

    private static void appendHeader(StringBuilder out, String name, String value) {
        if (name == null || value == null || name.indexOf(':') >= 0
                || name.indexOf('\r') >= 0 || name.indexOf('\n') >= 0
                || value.indexOf('\r') >= 0 || value.indexOf('\n') >= 0) {
            throw new IllegalArgumentException("Invalid intercepted response header");
        }
        out.append(name).append(": ").append(value).append("\r\n");
    }

    private static byte[] read(InputStream input) throws IOException {
        if (input == null) return new byte[0];
        try (ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
            InputStream stream = input;
            byte[] block = new byte[16384];
            int count;
            while ((count = stream.read(block)) != -1) {
                if (count == 0) {
                    int value = stream.read();
                    if (value < 0) break;
                    if (bytes.size() >= 64 * 1024 * 1024) {
                        throw new IOException("Intercepted worker response exceeds bridge capacity");
                    }
                    bytes.write(value);
                } else {
                    if (bytes.size() > 64 * 1024 * 1024 - count) {
                        throw new IOException("Intercepted worker response exceeds bridge capacity");
                    }
                    bytes.write(block, 0, count);
                }
            }
            return bytes.toByteArray();
        }
    }

    static final class Settings extends ServiceWorkerWebSettings {
        private final boolean mInternet;
        private volatile boolean mContent = true, mFile = true;
        private final boolean mBlock;
        Settings(boolean internet) { mInternet = internet; mBlock = !internet; }
        @Override public void setCacheMode(int mode) {
            if (mode != WebSettings.LOAD_DEFAULT) {
                throw new UnsupportedOperationException("WebView2 has no per-worker cache load flags");
            }
        }
        @Override public int getCacheMode() { return WebSettings.LOAD_DEFAULT; }
        @Override public void setAllowContentAccess(boolean allow) { mContent = allow; }
        @Override public boolean getAllowContentAccess() { return mContent; }
        @Override public void setAllowFileAccess(boolean allow) { mFile = allow; }
        @Override public boolean getAllowFileAccess() { return mFile; }
        @Override public void setBlockNetworkLoads(boolean block) {
            if (!block && !mInternet) throw new SecurityException("INTERNET permission required");
            if (block && mInternet) {
                throw new UnsupportedOperationException("WebView2 cannot cancel a worker load with an Android network error");
            }
        }
        @Override public boolean getBlockNetworkLoads() { return mBlock; }
    }

    static final class Request implements WebResourceRequest {
        private final Uri mUri;
        private final String mMethod;
        private final Map<String, String> mHeaders;
        Request(String url, String method, String headers) {
            mUri = Uri.parse(url);
            mMethod = method;
            Map<String, String> values = new LinkedHashMap<>();
            if (headers != null) for (String line : headers.split("\r\n")) {
                int colon = line.indexOf(':');
                if (colon > 0) values.put(line.substring(0, colon).trim(), line.substring(colon + 1).trim());
            }
            mHeaders = Collections.unmodifiableMap(values);
        }
        @Override public Uri getUrl() { return mUri; }
        @Override public boolean isForMainFrame() { return false; }
        @Override public boolean isRedirect() { return false; }
        @Override public boolean hasGesture() { return false; }
        @Override public String getMethod() { return mMethod; }
        @Override public Map<String, String> getRequestHeaders() { return mHeaders; }
    }
}
