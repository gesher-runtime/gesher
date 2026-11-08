/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.bridgecookies;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import java.io.File;
import java.util.UUID;

/** Android fragment-completion compatibility against an isolated loopback page. */
public final class NavigationActivity extends Activity {
    private static final class Step {
        final String label, script, suffix;
        final boolean finishes;
        Step(String label, String script, String suffix, boolean finishes) {
            this.label = label; this.script = script; this.suffix = suffix; this.finishes = finishes;
        }
    }
    // Chromium's Android NavigationHandleProxy classifies primary same-document
    // URLs equal ignoring their reference as fragment navigations. This includes
    // history-API changes to only the hash; a path/query change is different.
    private static final Step[] STEPS = {
        new Step("location-hash-close", "location.hash='close'", "/after#close", true),
        new Step("location-hash-other", "location.hash='other'", "/after#other", true),
        new Step("pushState-hash", "history.pushState({},'', '#pushed')", "/after#pushed", true),
        new Step("replaceState-hash", "history.replaceState({},'', '#replaced')", "/after#replaced", true),
        new Step("remove-hash", "history.replaceState({},'', '/after')", "/after", true),
        new Step("pushState-path", "history.pushState({},'', '/history-path')", "/history-path", false),
        new Step("pushState-query", "history.pushState({},'', '?fixture=1')", "/history-path?fixture=1", false),
        new Step("pushState-path-and-hash", "history.pushState({},'', '/another-path#changed')", "/another-path#changed", false),
        new Step("close-on-another-path", "location.hash='close'", "/another-path#close", true)
    };
    private final Handler main = new Handler(Looper.getMainLooper());
    private LoopbackServer server;
    private WebView web;
    private boolean terminal, loaded;
    private int started, finished, index = -1, startBaseline, finishBaseline;
    private String historyUrl;
    private long deadline;

    private static void say(String value) {
        System.out.println("bridge-navigation: " + value); System.out.flush();
    }
    private void fail(String reason) {
        if (terminal) return;
        terminal = true; say("FAIL " + reason); finish();
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            String run = UUID.randomUUID().toString().replace("-", "");
            Class.forName("com.gesher.app.webview.WebView2Host")
                    .getMethod("configure", String.class, String.class).invoke(null,
                            new File(getFilesDir(), "navigation-profile-" + run).getAbsolutePath(),
                            "--no-proxy-server --disable-background-networking --disable-component-update --no-first-run --disable-sync");
            server = new LoopbackServer(run);
            FrameLayout content = new FrameLayout(this); setContentView(content);
            web = new WebView(this); web.getSettings().setJavaScriptEnabled(true);
            content.addView(web, new FrameLayout.LayoutParams(-1, -1));
            web.setWebViewClient(new WebViewClient() {
                @Override public void onPageStarted(WebView view, String url, android.graphics.Bitmap icon) {
                    started++;
                }
                @Override public void doUpdateVisitedHistory(WebView view, String url, boolean reload) {
                    historyUrl = url;
                }
                @Override public void onPageFinished(WebView view, String url) {
                    finished++;
                    if (!loaded) {
                        if (!url.equals(server.origin() + "/after")) { fail("initial-url"); return; }
                        loaded = true;
                        main.postDelayed(NavigationActivity.this::beginNext, 150);
                    }
                }
            });
            web.loadUrl(server.origin() + "/after");
            main.postDelayed(() -> fail("overall-timeout"), 60000);
        } catch (Exception error) { fail("initialization"); }
    }

    private void beginNext() {
        if (terminal) return;
        if (index == -1 && (started != 1 || finished != 1)) { fail("initial-callback-counts"); return; }
        if (++index == STEPS.length) {
            if (server.posts.get() != 0 || server.bodyBytes.get() != 0 || server.reservedLeaks.get() != 0) {
                fail("unexpected-post-traffic"); return;
            }
            terminal = true;
            say("PASS fragment navigation and hash-only history changes finish once without page-start; path/query history changes do not finish");
            say("all passed"); finish(); return;
        }
        startBaseline = started; finishBaseline = finished; historyUrl = null;
        deadline = SystemClock.elapsedRealtime() + 5000;
        web.evaluateJavascript(STEPS[index].script, ignored -> { });
        main.postDelayed(this::poll, 25);
    }

    private void poll() {
        if (terminal) return;
        Step step = STEPS[index];
        int targetFinishes = finishBaseline + (step.finishes ? 1 : 0);
        if (started != startBaseline || finished > targetFinishes) { fail(step.label + "-extra-callback"); return; }
        String target = server.origin() + step.suffix;
        if (target.equals(historyUrl) && target.equals(web.getUrl()) && finished == targetFinishes) {
            main.postDelayed(() -> {
                if (terminal) return;
                if (started != startBaseline || finished != targetFinishes || !target.equals(web.getUrl())) {
                    fail(step.label + "-unstable-callbacks"); return;
                }
                say("PASS " + step.label);
                beginNext();
            }, 150);
            return;
        }
        if (SystemClock.elapsedRealtime() >= deadline) { fail(step.label + "-timeout"); return; }
        main.postDelayed(this::poll, 25);
    }

    @Override protected void onDestroy() {
        terminal = true;
        if (web != null) web.destroy();
        if (server != null) server.close();
        super.onDestroy();
    }
}
