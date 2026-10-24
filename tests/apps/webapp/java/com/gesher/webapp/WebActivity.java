/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.webapp;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.webkit.ConsoleMessage;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.JsResult;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.LinearLayout;

import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * The WebView test app (docs/webview.md; tests/windows/tracks/37-webview.ps1 reads what it
 * says on stdout). A red header View (100 px), a WebView on file:///android_asset/index.html
 * (300 px, green) and a WebView on https://example.com/ below. The asset page calls a
 * JavaScript interface, logs to the console, shows an alert, fetches a URL the
 * WebViewClient intercepts and navigates to one it overrides; evaluateJavascript reads
 * values back. Once both pages are loaded: phase 2 grows the header to 150 px (the asset
 * WebView moves down) and sets and reads a cookie; phase 3 hides the remote WebView.
 */
public class WebActivity extends Activity {
    private static final String TAG = "WebActivity";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private View mHeader;
    private WebView mAsset;
    private WebView mRemote;
    private boolean mAssetDone;
    private boolean mRemoteDone;
    private boolean mPhase2;

    private static void say(String message) {
        System.out.println(message);
        System.out.flush();
        Log.i(TAG, message);
    }

    /** window.GesherTest in the asset page. */
    public static final class Bridge {
        @JavascriptInterface
        public int add(int a, int b) {
            return a + b;
        }

        @JavascriptInterface
        public String greet(String who) {
            return "Hello " + who;
        }

        @JavascriptInterface
        public void report(String text) {
            say("WebActivity bridge report: " + text);
        }

        /** Not annotated: not callable from the page. */
        public String secret() {
            return "secret";
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.WHITE);
        mHeader = new View(this);
        mHeader.setBackgroundColor(Color.RED);
        root.addView(mHeader, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 100));

        mAsset = new WebView(this);
        mAsset.getSettings().setJavaScriptEnabled(true);
        mAsset.getSettings().setDomStorageEnabled(true);
        mAsset.addJavascriptInterface(new Bridge(), "GesherTest");
        mAsset.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageStarted(WebView view, String url, android.graphics.Bitmap icon) {
                say("WebActivity onPageStarted: " + url);
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                say("WebActivity onPageFinished: " + url);
                if (!url.startsWith("file:///android_asset/")) return;
                view.evaluateJavascript(
                        "(function() { return document.title + '|' + window.fromAppJs + '|'"
                                + " + (6 * 7); })()",
                        value -> {
                            say("WebActivity evaluateJavascript: " + value);
                            mAssetDone = true;
                            maybeNextPhase();
                        });
            }

            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                boolean take = "gesher.invalid".equals(request.getUrl().getHost());
                if (take) say("WebActivity shouldOverrideUrlLoading: " + request.getUrl());
                return take;
            }

            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view,
                    WebResourceRequest request) {
                if (!"intercepted.example".equals(request.getUrl().getHost())) return null;
                say("WebActivity shouldInterceptRequest: " + request.getUrl());
                Map<String, String> headers = new HashMap<>();
                headers.put("Access-Control-Allow-Origin", "*");
                return new WebResourceResponse("application/json", "UTF-8", 200, "OK", headers,
                        new ByteArrayInputStream("{\"value\": 42}".getBytes(StandardCharsets.UTF_8)));
            }
        });
        mAsset.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onConsoleMessage(ConsoleMessage message) {
                say("WebActivity console: " + message.message() + " (" + message.messageLevel()
                        + ")");
                return true;
            }

            @Override
            public void onReceivedTitle(WebView view, String title) {
                say("WebActivity onReceivedTitle: " + title);
            }

            @Override
            public void onProgressChanged(WebView view, int progress) {
                if (progress == 100) say("WebActivity progress 100");
            }

            @Override
            public boolean onJsAlert(WebView view, String url, String message, JsResult result) {
                say("WebActivity onJsAlert: " + message);
                result.confirm();
                return true;
            }
        });
        root.addView(mAsset, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 300));

        mRemote = new WebView(this);
        mRemote.getSettings().setJavaScriptEnabled(true);
        mRemote.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                say("WebActivity onPageFinished: " + url);
                view.evaluateJavascript("document.title", value -> {
                    say("WebActivity remote title: " + value);
                    mRemoteDone = true;
                    maybeNextPhase();
                });
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request,
                    android.webkit.WebResourceError error) {
                say("WebActivity onReceivedError: " + request.getUrl() + " "
                        + error.getErrorCode() + " " + error.getDescription());
            }
        });
        root.addView(mRemote, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(root);

        mAsset.loadUrl("file:///android_asset/index.html");
        Map<String, String> headers = new HashMap<>();
        headers.put("X-Gesher-Test", "1");
        mRemote.loadUrl("https://example.com/", headers);
        say("WebActivity onCreate: user agent " + mAsset.getSettings().getUserAgentString());
    }

    private void maybeNextPhase() {
        if (!mAssetDone || !mRemoteDone || mPhase2) return;
        mPhase2 = true;
        say("WebActivity phase 1 done: " + where());
        // Time for the test to look at the windows where they are.
        mHandler.postDelayed(this::phase2, 4000);
    }

    private void phase2() {
        mHeader.setLayoutParams(new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 150));
        say("WebActivity phase 2: header 150");
        CookieManager cookies = CookieManager.getInstance();
        cookies.setCookie("https://example.com/", "gesher=1; path=/", ok -> {
            say("WebActivity setCookie: " + ok);
            say("WebActivity cookie: " + cookies.getCookie("https://example.com/"));
            mHandler.postDelayed(this::phase3, 1500);
        });
    }

    private void phase3() {
        mRemote.setVisibility(View.INVISIBLE);
        mHandler.postDelayed(() -> say("WebActivity phase 3: remote hidden: " + where()), 1000);
    }

    /** "asset x,y wxh remote x,y wxh": where the WebViews are in the window. */
    private String where() {
        return "asset " + where(mAsset) + " remote " + where(mRemote);
    }

    private static String where(View v) {
        int[] at = new int[2];
        v.getLocationInWindow(at);
        return at[0] + "," + at[1] + " " + v.getWidth() + "x" + v.getHeight()
                + (v.getVisibility() == View.VISIBLE ? "" : " hidden");
    }

    @Override
    protected void onResume() {
        super.onResume();
        say("WebActivity onResume");
    }

    @Override
    protected void onDestroy() {
        say("WebActivity onDestroy");
        super.onDestroy();
    }
}
