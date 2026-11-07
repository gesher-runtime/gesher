/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.bridgecookies;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.webkit.ConsoleMessage;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import java.io.File;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicInteger;

/** Actual WebView2 request-source classification and reserved-request non-leakage. */
public final class WorkerActivity extends Activity {
    private final Handler main = new Handler(Looper.getMainLooper());
    private final AtomicInteger calls = new AtomicInteger();
    private volatile boolean terminal;
    private WorkerServer server;
    private WebView web;

    private static void say(String value) {
        System.out.println("bridgeworkers: " + value); System.out.flush();
    }
    private void fail(String phase) {
        main.post(() -> {
            if (terminal) return;
            terminal = true; say("FAIL " + phase.replaceAll("[^A-Za-z0-9_-]", "_")); finish();
        });
    }
    public final class Bridge {
        @JavascriptInterface public String bootstrap() { return "ready"; }
        @JavascriptInterface public int invocations() { return calls.get(); }
        @JavascriptInterface public String once(String marker) {
            if (!"worker-fixture-do-not-upload".equals(marker) || calls.incrementAndGet() != 1) {
                throw new IllegalStateException("Unexpected worker bridge invocation");
            }
            if (Looper.myLooper() == Looper.getMainLooper()) throw new IllegalStateException("UI bridge");
            try {
                if ((Boolean) Class.forName("com.gesher.app.webview.WebView2Host")
                        .getDeclaredMethod("onWebViewThread").invoke(null)) {
                    throw new IllegalStateException("STA bridge");
                }
            } catch (ReflectiveOperationException error) { throw new IllegalStateException("Worker check"); }
            return "dedicated-ok";
        }
        @JavascriptInterface public void failed(String phase) { fail("javascript-" + phase); }
        @JavascriptInterface public void finished() {
            main.postDelayed(() -> {
                if (terminal) return;
                if (calls.get() != 1 || server.posts.get() != 0 || server.bodyBytes.get() != 0
                        || server.reserved.get() != 0 || server.privatePayloadLeaks.get() != 0) {
                    fail("worker-payload-reached-origin-or-Java"); return;
                }
                terminal = true;
                say("PASS dedicated worker delegated POST returns 200 and invokes Java exactly once");
                say("PASS shared and service worker POST requests return 403 without Java invocation");
                say("PASS HTTP origin received zero POST requests, body bytes or reserved paths");
                say("all passed"); finish();
            }, 300);
        }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            Class.forName("com.gesher.app.webview.WebView2Host").getMethod("configure", String.class, String.class)
                    .invoke(null, new File(getFilesDir(), "worker-profile-" + UUID.randomUUID()).getAbsolutePath(),
                            "--no-proxy-server --disable-background-networking --disable-component-update --no-first-run --disable-sync");
            server = new WorkerServer(); web = new WebView(this); setContentView(web);
            web.getSettings().setJavaScriptEnabled(true);
            web.setWebChromeClient(new WebChromeClient() {
                @Override public boolean onConsoleMessage(ConsoleMessage message) {
                    if (message.message().startsWith("bridgeworkers FAIL ")) fail("javascript-console");
                    return true;
                }
            });
            web.addJavascriptInterface(new Bridge(), "Fixture");
            web.loadUrl(server.origin() + "/start");
            main.postDelayed(() -> fail("overall-timeout"), 60000);
        } catch (Exception error) { fail("initialization"); }
    }
    @Override protected void onDestroy() {
        terminal = true;
        if (web != null) web.destroy();
        if (server != null) server.close();
        super.onDestroy();
    }
}
