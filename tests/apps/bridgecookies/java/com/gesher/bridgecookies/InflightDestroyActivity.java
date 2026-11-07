/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.bridgecookies;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONObject;

/** Destroy a view while its genuine synchronous bridge request is still deferred. */
public final class InflightDestroyActivity extends Activity {
    private final Handler main = new Handler(Looper.getMainLooper());
    private final AtomicInteger entered = new AtomicInteger();
    private final AtomicBoolean returned = new AtomicBoolean();
    private final CountDownLatch destroyed = new CountDownLatch(1);
    private volatile boolean terminal;
    private LoopbackServer server;
    private FrameLayout content;
    private WebView web;
    private String oldEndpoint;
    private String oldBody;

    private static void say(String value) {
        System.out.println("bridge-inflight: " + value); System.out.flush();
    }
    private void fail(String phase) {
        main.post(() -> {
            if (terminal) return;
            terminal = true; say("FAIL " + phase); finish();
        });
    }
    public final class Bridge {
        @JavascriptInterface public String capture() { return "ready"; }
        @JavascriptInterface public void begin(String marker, String endpoint, String body) {
            if (Looper.myLooper() == Looper.getMainLooper() || entered.incrementAndGet() != 1
                    || !"fixture-inflight-private-argument-do-not-upload".equals(marker)
                    || endpoint == null || !endpoint.startsWith("/.gesher-js-bridge/")
                    || body == null || body.length() < 24 || body.length() > 256 || body.contains("fixture-")) {
                fail("invalid-callback"); return;
            }
            oldEndpoint = endpoint;
            oldBody = body;
            main.post(InflightDestroyActivity.this::destroyDuringCall);
            boolean interrupted = false;
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            try {
                // UI destroys the native host before this method is allowed to return.
                while (destroyed.getCount() != 0) {
                    long remaining = deadline - System.nanoTime();
                    if (remaining <= 0) { fail("destroy-not-reached"); return; }
                    try { destroyed.await(remaining, TimeUnit.NANOSECONDS); }
                    catch (InterruptedException expected) {
                        // shutdownNow interrupts before WebView.destroy has finished.
                        // Keep waiting until UI has actually requested native teardown.
                        interrupted = true;
                    }
                }
            } finally {
                returned.set(true);
                if (interrupted) Thread.currentThread().interrupt();
            }
        }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            server = new LoopbackServer(UUID.randomUUID().toString().replace("-", ""));
            content = new FrameLayout(this); setContentView(content);
            web = view(); web.addJavascriptInterface(new Bridge(), "Fixture");
            web.setWebViewClient(new WebViewClient() {
                private boolean started;
                @Override public void onPageFinished(WebView view, String url) {
                    if (started || !url.endsWith("/after")) return;
                    started = true;
                    view.evaluateJavascript("(function(){var endpoint=null,firstBody=null;var open=XMLHttpRequest.prototype.open;"
                            + "var send=XMLHttpRequest.prototype.send;XMLHttpRequest.prototype.send=function(body){"
                            + "if(firstBody===null)firstBody=body;return send.apply(this,arguments);};"
                            + "XMLHttpRequest.prototype.open=function(m,u,a){var p=new URL(u,location.href);"
                            + "if(p.pathname.indexOf('/.gesher-js-bridge/')===0)endpoint=p.pathname;"
                            + "return open.apply(this,arguments);};"
                            + "if(Fixture.capture()!=='ready'||!endpoint)throw new Error('capture');"
                            + "Fixture.begin('fixture-inflight-private-argument-do-not-upload',endpoint,firstBody);})()", ignored -> { });
                }
            });
            web.loadUrl(server.origin() + "/after");
            main.postDelayed(() -> fail("overall-timeout"), 30000);
        } catch (Exception error) { fail("initialization"); }
    }

    private WebView view() {
        WebView result = new WebView(this);
        result.getSettings().setJavaScriptEnabled(true);
        content.addView(result, new FrameLayout.LayoutParams(-1, -1));
        return result;
    }
    private void destroyDuringCall() {
        content.removeView(web);
        web.destroy();
        destroyed.countDown();
        web = view();
        web.setWebViewClient(new WebViewClient() {
            private boolean tested;
            @Override public void onPageFinished(WebView view, String url) {
                if (tested || !url.endsWith("/after")) return;
                tested = true;
                String script = "(function(){try{var x=new XMLHttpRequest();x.open('POST',"
                        + JSONObject.quote(oldEndpoint) + ",false);x.send(" + JSONObject.quote(oldBody)
                        + ");return x.status===403?'denied':'wrong';}catch(e){return 'failed';}})()";
                view.evaluateJavascript(script, response -> {
                    if (!"\"denied\"".equals(response)) { fail("old-endpoint-not-denied"); return; }
                    main.postDelayed(() -> {
                        if (terminal) return;
                        say("origin counters: posts=" + server.posts.get() + " bodyBytes="
                                + server.bodyBytes.get() + " reserved=" + server.reservedLeaks.get()
                                + " privatePayloadLeaks=" + server.privatePayloadLeaks.get());
                        if (!returned.get()) { fail("callback-not-returned"); return; }
                        if (entered.get() != 1) { fail("callback-entry-count-" + entered.get()); return; }
                        if (server.posts.get() != 0) { fail("origin-post-count-" + server.posts.get()); return; }
                        if (server.bodyBytes.get() != 0) { fail("origin-body-count-" + server.bodyBytes.get()); return; }
                        if (server.reservedLeaks.get() != 0) { fail("origin-reserved-count-" + server.reservedLeaks.get()); return; }
                        if (server.privatePayloadLeaks.get() != 0) { fail("private-arguments-reached-origin"); return; }
                        terminal = true;
                        say("PASS native host destroyed while Java callback was in flight; late callback returned safely");
                        say("PASS old endpoint rejected without Java reentry or HTTP origin traffic");
                        say("all passed"); finish();
                    }, 300);
                });
            }
        });
        web.loadUrl(server.origin() + "/after");
    }
    @Override protected void onDestroy() {
        terminal = true;
        destroyed.countDown();
        if (web != null) web.destroy();
        if (server != null) server.close();
        super.onDestroy();
    }
}
