/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.web3app;

import android.os.Handler;
import android.os.Looper;
import android.webkit.ValueCallback;
import android.webkit.WebView;

/** What the web3 test activities share: their "Web3 ..." log lines and the test's ports. */
final class Web3 {
    private Web3() {
    }

    static void log(String s) {
        System.out.println("Web3 " + s);
        System.out.flush();
    }

    /** $GESHER_WEB3_PORTS: "proxyPort httpPort" of the test's servers (54-web3.ps1). */
    static int port(int index) {
        String v = System.getenv("GESHER_WEB3_PORTS");
        if (v == null) return 0;
        String[] f = v.trim().split("\\s+");
        try {
            return index < f.length ? Integer.parseInt(f[index]) : 0;
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /**
     * Evaluates {@code script} every 250 ms until its JSON result satisfies {@code done}
     * (or {@code seconds} pass: then the last result), then calls {@code then} on the UI
     * thread.
     */
    static void poll(WebView view, String script, java.util.function.Predicate<String> done,
            int seconds, ValueCallback<String> then) {
        Handler h = new Handler(Looper.getMainLooper());
        long deadline = android.os.SystemClock.uptimeMillis() + seconds * 1000L;
        Runnable[] step = new Runnable[1];
        step[0] = () -> view.evaluateJavascript(script, value -> {
            if ((value != null && done.test(value))
                    || android.os.SystemClock.uptimeMillis() > deadline) {
                then.onReceiveValue(value);
            } else {
                h.postDelayed(step[0], 250);
            }
        });
        h.post(step[0]);
    }
}
