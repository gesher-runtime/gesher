/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app.webview;

/** Host-only WebView2 completion boundary. The production CookieManager is compiled unchanged. */
public final class WebView2Host {
    public interface GlobalCallback { void onResult(int type, int result, String value); }
    public static boolean available = true;
    public static boolean immediate;
    private static boolean complete(GlobalCallback callback) {
        if (!available) return false;
        Runnable result = () -> callback.onResult(2, 0, null);
        if (immediate) result.run();
        else {
            Thread worker = new Thread(result, "cookie-native-completion-without-looper");
            worker.setDaemon(true);
            worker.start();
        }
        return true;
    }
    public static boolean setCookie(String u, String n, String v, String d, String p,
            double e, boolean s, boolean h, GlobalCallback callback) { return complete(callback); }
    public static boolean deleteCookies(int which, GlobalCallback callback) { return complete(callback); }
    public static boolean getCookies(String url, GlobalCallback callback) { return false; }
    public static boolean onWebViewThread() { return false; }
}
