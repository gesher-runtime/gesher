/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.webapp;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebStorage;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.util.Collections;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicIntegerArray;
import java.util.function.LongConsumer;

/** Actual ART/WebView2 storage via public Android APIs; only the page response is a fixture. */
public final class WebStorageActivity extends Activity {
    private static final int PAYLOAD_BYTES = 256 * 1024;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final AtomicIntegerArray served = new AtomicIntegerArray(2);
    private final String[] origins = new String[2];
    private final String[] urls = new String[2];
    private WebView web;
    private WebStorage storage;
    private byte[] page;
    private String token;
    private String phase = "setup";
    private String loadingUrl;
    private Runnable pageLoaded;
    private Runnable pageTimeout;
    private Runnable pageNext;
    private int pageId;
    private int pageOrigin;
    private String pageExpected;
    private boolean pagePending;
    private boolean done;
    private boolean failed;
    private int queries;
    private int callbacks;

    private void say(String text) {
        System.out.println("webstorage: " + text);
        System.out.flush();
    }

    private void fail(Throwable error) {
        if (failed) return;
        failed = true;
        done = true;
        handler.removeCallbacksAndMessages(null);
        say("FAIL phase=" + phase + " queries=" + queries + " callbacks=" + callbacks + " " + error);
        error.printStackTrace();
        finish();
    }

    private void checked(Runnable action) {
        if (done) return;
        try { action.run(); } catch (Throwable error) { fail(error); }
    }

    private static void require(boolean condition, String detail) {
        if (!condition) throw new AssertionError(detail);
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        checked(() -> {
            token = UUID.randomUUID().toString().replace("-", "");
            for (int i = 0; i < origins.length; i++) {
                // Different fresh hosts, also different sites; no asset/about:blank origin.
                origins[i] = "https://gesher-storage-" + (i == 0 ? "a-" : "b-") + token + ".invalid";
                urls[i] = origins[i] + "/storage.html";
                say("origin " + label(i) + "=" + origins[i]);
            }
            try (InputStream input = getAssets().open("storage.html")) {
                page = input.readAllBytes();
            } catch (Exception error) { throw new AssertionError("Reading storage fixture", error); }
            require(page.length > 0, "Empty storage fixture");
            web = new WebView(this);
            web.getSettings().setJavaScriptEnabled(true);
            web.getSettings().setDomStorageEnabled(true);
            web.addJavascriptInterface(new Bridge(), "StorageResult");
            web.setWebViewClient(new WebViewClient() {
                @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                    String url = request.getUrl().toString();
                    for (int i = 0; i < urls.length; i++) {
                        if (!urls[i].equals(url)) continue;
                        if (!request.isForMainFrame() || !"GET".equals(request.getMethod())) {
                            handler.post(() -> fail(new AssertionError("Unexpected fixture request " + url)));
                            return null;
                        }
                        served.incrementAndGet(i);
                        return new WebResourceResponse("text/html", "UTF-8", 200, "OK",
                                Collections.singletonMap("Cache-Control", "no-store"), new ByteArrayInputStream(page));
                    }
                    return null;
                }

                @Override public void onPageFinished(WebView view, String url) {
                    if (done || !url.equals(loadingUrl) || pageLoaded == null) return;
                    Runnable next = pageLoaded;
                    pageLoaded = null;
                    checked(next);
                }

                @Override public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                    if (request.isForMainFrame()) handler.post(() -> fail(new AssertionError(
                            "Main page " + request.getUrl() + " error=" + error.getErrorCode() + " " + error.getDescription())));
                }
            });
            setContentView(web);
            storage = WebStorage.getInstance();
            handler.postDelayed(() -> fail(new AssertionError("Total 120-second deadline exceeded")), 120_000);
            runPage(0, "write", "written", () -> runPage(1, "write", "written", () ->
                    awaitMeasurement(0, false, "baseline A", () ->
                    awaitMeasurement(1, false, "baseline B", this::deleteFirstOrigin))));
        });
    }

    private static String label(int index) { return index == 0 ? "A" : "B"; }

    private void runPage(int index, String operation, String expected, Runnable next) {
        phase = operation + " " + label(index);
        int id = ++pageId;
        pageOrigin = index;
        pageExpected = expected;
        pageNext = next;
        pagePending = true;
        loadingUrl = urls[index];
        pageTimeout = () -> fail(new AssertionError("Page/IndexedDB callback timed out id=" + id + " url=" + loadingUrl));
        handler.postDelayed(pageTimeout, 30_000);
        pageLoaded = () -> {
            require(served.get(index) > 0, "Top-level fixture was not intercepted: " + loadingUrl);
            // Arguments contain only the generated URL/token and fixed test vocabulary.
            web.evaluateJavascript("storageFixture(" + id + ",'" + operation + "','" + origins[index]
                    + "','" + label(index) + "-" + token + "')", null);
        };
        web.loadUrl(loadingUrl);
    }

    public final class Bridge {
        @JavascriptInterface public void result(int id, String actualOrigin, String outcome, int bytes, String detail) {
            handler.post(() -> checked(() -> {
                require(pagePending && id == pageId, "Duplicate/stale page callback id=" + id + " expected=" + pageId);
                require(origins[pageOrigin].equals(actualOrigin), "Wrong browser origin " + actualOrigin);
                require(pageExpected.equals(outcome), "IndexedDB " + outcome + ": " + detail);
                require(bytes == ("absent".equals(outcome) ? 0 : PAYLOAD_BYTES), "Wrong payload size " + bytes);
                require("closed".equals(detail), "IndexedDB connection still open: " + detail);
                pagePending = false;
                handler.removeCallbacks(pageTimeout);
                say("PASS " + outcome + " " + label(pageOrigin) + " bytes=" + bytes + " connection=closed origin=" + actualOrigin);
                Runnable next = pageNext;
                pageNext = null;
                next.run();
            }));
        }
    }

    /** One public API call at a time; every delivery must be unique and on the invoking UI thread. */
    private void query(int index, boolean quota, LongConsumer next) {
        int id = ++queries;
        String operation = quota ? "quota" : "usage";
        AtomicInteger deliveries = new AtomicInteger();
        Runnable timeout = () -> fail(new AssertionError("Public " + operation + " callback timed out id=" + id
                + " origin=" + origins[index] + " deliveries=" + deliveries.get()));
        handler.postDelayed(timeout, 10_000);
        android.webkit.ValueCallback<Long> callback = value -> {
            int count = deliveries.incrementAndGet();
            boolean onMain = Looper.myLooper() == Looper.getMainLooper();
            String thread = Thread.currentThread().getName();
            if (count != 1 || !onMain) {
                AssertionError error = new AssertionError("Public " + operation + " callback id=" + id
                        + " count=" + count + " thread=" + thread);
                // Log before posting: final UI cleanup must not hide a queued invalid delivery.
                say("FAIL " + error);
                handler.post(() -> fail(error));
                return;
            }
            checked(() -> {
                handler.removeCallbacks(timeout);
                callbacks++;
                require(value != null && value >= 0, "Invalid public " + operation + " result " + value);
                next.accept(value);
            });
        };
        if (quota) storage.getQuotaForOrigin(origins[index], callback);
        else storage.getUsageForOrigin(origins[index], callback);
    }

    private void awaitMeasurement(int index, boolean empty, String name, Runnable next) {
        phase = name;
        pollMeasurement(index, empty, name, SystemClock.uptimeMillis() + 20_000, next);
    }

    private void pollMeasurement(int index, boolean empty, String name, long deadline, Runnable next) {
        require(SystemClock.uptimeMillis() < deadline, "Quota-manager polling exceeded 20 seconds: " + name);
        query(index, false, usage -> query(index, true, quota -> {
            say("measurement " + name + " usage=" + usage + " quota=" + quota);
            require(SystemClock.uptimeMillis() <= deadline, "Quota-manager result exceeded 20 seconds: " + name
                    + " usage=" + usage + " quota=" + quota);
            if ((empty ? usage == 0 : usage > 0) && quota > 0) {
                say("PASS " + name + " usage=" + usage + " quota=" + quota);
                next.run();
            } else {
                handler.postDelayed(() -> checked(() -> pollMeasurement(index, empty, name, deadline, next)), 250);
            }
        }));
    }

    private void deleteFirstOrigin() {
        phase = "delete A";
        // Both writing transactions have completed and their IDB connections are closed.
        // The currently displayed page belongs to B, so this also checks origin routing.
        storage.deleteOrigin(origins[0]);
        // No reopening A until the public query observes zero usage and positive quota.
        awaitMeasurement(0, true, "deleted A", () ->
                runPage(0, "read", "absent", () ->
                runPage(1, "read", "present", () ->
                awaitMeasurement(1, false, "preserved B", this::success))));
    }

    private void success() {
        require(queries == callbacks, "Public callback imbalance " + queries + "/" + callbacks);
        require(served.get(0) >= 2 && served.get(1) >= 2, "Each origin must be loaded before and after deletion");
        done = true;
        handler.removeCallbacksAndMessages(null);
        say("ALL PASSED origins=2 public_queries=" + queries + " callbacks=" + callbacks + " callbacks_on_main=true");
        finish();
    }

    @Override protected void onDestroy() {
        done = true;
        handler.removeCallbacksAndMessages(null);
        if (web != null) { web.removeJavascriptInterface("StorageResult"); web.destroy(); }
        super.onDestroy();
    }
}
