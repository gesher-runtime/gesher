/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.bridgecookies;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.webkit.ConsoleMessage;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import java.io.File;
import java.lang.reflect.Method;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONObject;

/** Actual WebView2 bridge worker + HttpOnly cookie regression against its own loopback origin. */
public final class BridgeCookiesActivity extends Activity {
    private static final AtomicBoolean FAILED = new AtomicBoolean();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final AtomicInteger order = new AtomicInteger(), entries = new AtomicInteger();
    private final AtomicBoolean terminal = new AtomicBoolean();
    private long workerId = -1;
    private LoopbackServer server;
    private FrameLayout content;
    private WebView web;
    private Method onWebViewThread;

    private static void say(String value) { System.out.println("bridgecookies: " + value); System.out.flush(); }
    static void fail(String label) { FAILED.set(true); say("FAIL " + label.replaceAll("[^A-Za-z0-9_-]", "_")); }
    private static void require(boolean value, String label) { if (!value) throw new AssertionError(label); }

    private void worker(int sequence) {
        entries.incrementAndGet();
        try {
            require(Looper.myLooper() != Looper.getMainLooper(), "bridge ran on Android UI thread");
            require(!(Boolean) onWebViewThread.invoke(null), "bridge ran on native WebView2 STA");
            long id = Thread.currentThread().getId();
            if (workerId < 0) workerId = id;
            require(workerId == id, "bridge did not use one ordered Java worker");
            require(order.incrementAndGet() == sequence, "bridge method ordering changed");
        } catch (ReflectiveOperationException error) { throw new AssertionError("Cannot verify bridge worker", error); }
    }

    public final class Bridge {
        @JavascriptInterface public void recordVoid(int sequence, String marker) {
            worker(sequence);
            require("fixture-bridge-argument-do-not-upload".equals(marker), "void payload changed");
            try { Thread.sleep(120); } catch (InterruptedException error) { Thread.currentThread().interrupt(); throw new AssertionError(error); }
        }
        @JavascriptInterface public int add(int sequence, int left, int right) { worker(sequence); return left + right; }
        @JavascriptInterface public String cookies(int sequence, String documentCookies) {
            worker(sequence);
            require(documentCookies != null && documentCookies.contains(server.visibleCookie() + "=fixture-visible")
                    && !documentCookies.contains(server.hiddenCookie()), "document.cookie exposed HttpOnly or lost visible cookie");
            String actual = CookieManager.getInstance().getCookie(server.origin() + "/start");
            require(actual != null && actual.contains(server.hiddenCookie() + "=fixture-hidden")
                    && actual.contains(server.visibleCookie() + "=fixture-visible"), "bridge worker could not read real HttpOnly cookies");
            say("PASS real HttpOnly CookieManager callback read; document.cookie hides it");
            return "http-only-ok";
        }
        @JavascriptInterface public int large(int sequence, String value) {
            worker(sequence); require(value.startsWith("fixture-bridge-argument-do-not-upload:"), "large payload changed"); return value.length();
        }
        @JavascriptInterface public String largeResult(int sequence) { worker(sequence); return "r".repeat(262144); }
        @JavascriptInterface public void expectedFailure(int sequence) { worker(sequence); throw new IllegalStateException("expected local fixture failure"); }
        @JavascriptInterface public void failed(String phase) { fail(phase); main.post(BridgeCookiesActivity.this::finish); }
        @JavascriptInterface public void finished(int sequence, String endpoint, String firstBody) {
            worker(sequence);
            require(endpoint != null && endpoint.startsWith("/.gesher-js-bridge/") && endpoint.indexOf('?') < 0,
                    "invalid captured bridge endpoint");
            require(firstBody != null && firstBody.length() >= 24 && firstBody.length() <= 256
                    && !firstBody.contains("fixture-") && firstBody.indexOf('{') < 0
                    && firstBody.indexOf('[') < 0 && firstBody.indexOf('"') < 0,
                    "HTTP bridge body was not an opaque nonce");
            say("PASS synchronous void/nonvoid ordering on one non-STA Java bridge worker");
            say("PASS large input/output and Java-exception recovery");
            say("PASS HTTP request body carries only an opaque nonce while Java receives the private arguments");
            say("PASS consumed and explicitly cancelled nonces cannot invoke Java again");
            say("PASS malformed UTF-8, oversized body, wrong method/query and unregistered endpoint fail closed");
            main.post(() -> destroyedEndpoint(endpoint, firstBody));
        }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        FAILED.set(false);
        try {
            String run = UUID.randomUUID().toString().replace("-", "");
            // Explicit fresh profile before the first WebView/CookieManager exists.
            Class<?> host = Class.forName("com.gesher.app.webview.WebView2Host");
            host.getMethod("configure", String.class, String.class).invoke(null,
                    new File(getFilesDir(), "bridge-profile-" + run).getAbsolutePath(),
                    "--disable-background-networking --disable-component-update --no-first-run --disable-sync");
            onWebViewThread = host.getDeclaredMethod("onWebViewThread"); onWebViewThread.setAccessible(true);
            server = new LoopbackServer(run);
            content = new FrameLayout(this); setContentView(content);
            web = view(); web.addJavascriptInterface(new Bridge(), "Fixture");
            web.loadUrl(server.origin() + "/start");
            main.postDelayed(() -> { if (!terminal.get()) { fail("timeout"); finish(); } }, 60000);
        } catch (Exception error) { fail("initialization"); finish(); }
    }

    private WebView view() {
        WebView result = new WebView(this);
        result.getSettings().setJavaScriptEnabled(true);
        result.setWebChromeClient(new WebChromeClient() {
            @Override public boolean onConsoleMessage(ConsoleMessage message) {
                if (message.message().startsWith("bridgecookies FAIL ")) { fail("javascript-console"); main.post(BridgeCookiesActivity.this::finish); }
                return true;
            }
        });
        content.addView(result, new FrameLayout.LayoutParams(-1, -1));
        return result;
    }

    private void destroyedEndpoint(String endpoint, String body) {
        if (FAILED.get()) { finish(); return; }
        int completedEntries = entries.get();
        content.removeView(web); web.destroy();
        web = view();
        web.setWebViewClient(new WebViewClient() {
            private boolean tested;
            @Override public void onPageFinished(WebView view, String url) {
                if (tested || !url.endsWith("/after")) return;
                tested = true;
                String script = "(function(){try{var x=new XMLHttpRequest();x.open('POST',"
                        + JSONObject.quote(endpoint) + ",false);x.send(" + JSONObject.quote(body)
                        + ");return x.status===403?'stale-blocked':'bad-status';}catch(e){return 'request-failed';}})()";
                view.evaluateJavascript(script, result -> {
                    if (!"\"stale-blocked\"".equals(result) || entries.get() != completedEntries) { fail("destroyed-endpoint"); finish(); return; }
                    main.postDelayed(() -> {
                        if (server.posts.get() != 0 || server.bodyBytes.get() != 0 || server.reservedLeaks.get() != 0
                                || server.privatePayloadLeaks.get() != 0) {
                            fail("bridge-payload-reached-http-origin");
                        } else {
                            say("PASS destroyed view endpoint denied without old Java invocation");
                            say("PASS local HTTP origin received zero POST requests, body bytes or reserved bridge requests");
                            if (!FAILED.get()) say("all passed");
                        }
                        terminal.set(true); finish();
                    }, 300);
                });
            }
        });
        web.loadUrl(server.origin() + "/after");
    }
    @Override protected void onDestroy() {
        terminal.set(true);
        if (web != null) web.destroy();
        if (server != null) server.close();
        super.onDestroy();
    }
}
