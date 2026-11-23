/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app.webview;

import android.app.ActivityThread;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.webkit.ValueCallback;

import org.json.JSONObject;

import java.util.concurrent.atomic.AtomicBoolean;

/** Real Chromium quota-manager values; zero is the Android query-failure result. */
final class WebStorageQueries {
    private WebStorageQueries() { }

    static void query(String origin, boolean quota, ValueCallback<Long> callback) {
        if (callback == null) return;
        Looper looper = Looper.myLooper();
        Handler handler = new Handler(looper != null ? looper : Looper.getMainLooper());
        AtomicBoolean delivered = new AtomicBoolean();
        java.util.function.LongConsumer finish = value -> {
            if (delivered.compareAndSet(false, true)) handler.post(() -> callback.onReceiveValue(value));
        };
        try {
            Context app = ActivityThread.currentApplication();
            if (app != null) GesherWebView.configure(app);
            boolean accepted = WebView2Host.queryOriginStorage(origin, (type, hr, value) -> {
                finish.accept(type == 3 && hr >= 0 ? measurement(value, quota) : 0L);
            });
            if (!accepted) finish.accept(0L);
        } catch (RuntimeException failure) {
            finish.accept(0L);
        }
    }

    static long measurement(String json, boolean quota) {
        try {
            Object field = new JSONObject(json).get(quota ? "quota" : "usage");
            if (!(field instanceof Number)) return 0L;
            if (field instanceof Long) return Math.max(0L, (Long) field);
            double value = ((Number) field).doubleValue();
            if (!Double.isFinite(value) || value < 0 || value >= 0x1p63
                    || value != Math.floor(value)) return 0L;
            return ((Number) field).longValue();
        } catch (Exception failure) {
            return 0L;
        }
    }
}
