/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app.webview;

import android.content.Context;
import android.os.Handler;
import android.webkit.WebChromeClient;

/** Host-only WebView shell. Production GeolocationShim and framework queues are unchanged. */
final class GesherWebViewProvider {
    final WebView2Host host = new WebView2Host();
    private final Context context;
    private final Handler handler;
    GesherWebViewProvider(Context context, Handler handler) { this.context = context; this.handler = handler; }
    Context context() { return context; }
    Handler uiHandler() { return handler; }
    WebView2Host host() { return host; }
    WebChromeClient chrome() { return null; }
    Settings settings() { return new Settings(); }
    String currentUrl() { return "https://example.test/"; }
    static String originOf(String url) { return url; }
    static final class Settings { boolean geolocationEnabled() { return true; } }
}
