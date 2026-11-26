// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
package com.gesher.sandboxnetwork;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.Proxy;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

public final class ProbeActivity extends Activity {
    private static final String TAG = "sandbox-network-apk";
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final List<Endpoint> endpoints = new ArrayList<>();
    private WebView web;
    private int index;

    private record Endpoint(String name, URL url, String marker) {}

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        try (var reader = new BufferedReader(new InputStreamReader(
                getAssets().open("endpoints.tsv"), StandardCharsets.UTF_8))) {
            for (String line; (line = reader.readLine()) != null;) {
                String[] fields = line.split("\t", -1);
                if (fields.length != 3 || fields[2].isEmpty()) {
                    throw new IOException("Invalid endpoint fixture");
                }
                endpoints.add(new Endpoint(fields[0], new URL(fields[1]), fields[2]));
            }
            if (endpoints.isEmpty()) throw new IOException("Missing endpoints");
        } catch (IOException failure) {
            Log.e(TAG, "invalid fixture", failure);
            finish();
            return;
        }
        new Thread(() -> {
            for (Endpoint endpoint : endpoints) checkHttp(endpoint);
            handler.post(() -> {
                if (getIntent().getBooleanExtra("httpOnly", false)) complete();
                else checkNextWebView();
            });
        }, "sandbox-network-http").start();
    }

    private static void checkHttp(Endpoint endpoint) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) endpoint.url().openConnection(Proxy.NO_PROXY);
            connection.setConnectTimeout(5000);
            connection.setReadTimeout(5000);
            connection.setInstanceFollowRedirects(false);
            connection.setUseCaches(false);
            int status = connection.getResponseCode();
            if (status != 200) {
                report("http", endpoint, "unexpected-status-" + status);
                return;
            }
            try (var input = connection.getInputStream()) {
                String body = new String(input.readNBytes(8192), StandardCharsets.UTF_8);
                report("http", endpoint, body.contains(endpoint.marker())
                        ? "content" : "unexpected-content");
            }
        } catch (IOException failure) {
            report("http", endpoint, "failed-" + failure.getClass().getSimpleName());
        } catch (RuntimeException failure) {
            Log.e(TAG, "HTTP client failed for " + endpoint.name(), failure);
            report("http", endpoint, "client-failed");
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    private void checkNextWebView() {
        if (index == endpoints.size()) {
            complete();
            return;
        }
        Endpoint endpoint = endpoints.get(index);
        try {
            web = new WebView(this);
            setContentView(web);
            web.getSettings().setJavaScriptEnabled(true);
            WebView view = web;
            Runnable timeout = () -> endWebView(view, endpoint, "timeout");
            web.setWebViewClient(new WebViewClient() {
                @Override
                public void onPageFinished(WebView finished, String url) {
                    if (finished != web || !endpoint.url().toString().equals(url)) return;
                    String script = "(document.title + '\\n' + (document.body ? document.body.textContent : ''))"
                            + ".includes(" + JSONObject.quote(endpoint.marker()) + ")";
                    finished.evaluateJavascript(script, result -> {
                        handler.removeCallbacks(timeout);
                        endWebView(finished, endpoint,
                                "true".equals(result) ? "content" : "unexpected-content");
                    });
                }

                @Override
                public void onReceivedError(WebView failed, WebResourceRequest request,
                                            WebResourceError error) {
                    if (failed != web || !request.isForMainFrame()
                            || !endpoint.url().toString().equals(request.getUrl().toString())) return;
                    handler.removeCallbacks(timeout);
                    endWebView(failed, endpoint, "failed-" + error.getErrorCode());
                }
            });
            handler.postDelayed(timeout, 15000);
            web.loadUrl(endpoint.url().toString());
        } catch (RuntimeException failure) {
            Log.e(TAG, "WebView setup failed for " + endpoint.name(), failure);
            report("webview", endpoint, "setup-failed");
            index++;
            handler.post(this::checkNextWebView);
        }
    }

    private void endWebView(WebView view, Endpoint endpoint, String result) {
        if (view != web || isFinishing()) return;
        web = null;
        report("webview", endpoint, result);
        setContentView(new android.widget.FrameLayout(this));
        view.destroy();
        index++;
        handler.post(this::checkNextWebView);
    }

    private static void report(String client, Endpoint endpoint, String result) {
        Log.i(TAG, client + " " + endpoint.name() + " " + result);
    }

    private void complete() {
        Log.i(TAG, "completed");
        finish();
    }
}
