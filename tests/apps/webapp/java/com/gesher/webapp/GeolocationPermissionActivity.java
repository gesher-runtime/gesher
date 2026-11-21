/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.webapp;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.webkit.GeolocationPermissions;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/** A real page in a declared-but-ungranted app must fail before origin consent. */
public final class GeolocationPermissionActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private WebView web;
    private boolean done;
    private int prompts;
    private final Runnable timeout = () -> { throw new AssertionError("Geolocation permission result timed out"); };
    private void requireDenied() {
        if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_DENIED
                || checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_DENIED) {
            throw new AssertionError("Negative page requires actual denied app permissions");
        }
    }
    public final class Bridge {
        @JavascriptInterface public void result(int error) {
            handler.post(() -> {
                requireDenied();
                if (done || error != 1 || prompts != 0) throw new AssertionError(
                        "Denied application reached origin consent or a location: error=" + error + " prompts=" + prompts);
                done = true; handler.removeCallbacks(timeout);
                System.out.println("GeolocationPermission denied before origin prompt uid "
                        + android.os.Process.myUid() + " pid " + android.os.Process.myPid());
                System.out.flush(); finish();
            });
        }
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state); requireDenied();
        web = new WebView(this); web.getSettings().setJavaScriptEnabled(true);
        web.getSettings().setGeolocationEnabled(true);
        web.addJavascriptInterface(new Bridge(), "PermissionResult");
        web.setWebChromeClient(new WebChromeClient() {
            @Override public void onGeolocationPermissionsShowPrompt(String origin, GeolocationPermissions.Callback answer) {
                prompts++;
                answer.invoke(origin, false, false);
                throw new AssertionError("An ungranted app must not reach the origin prompt");
            }
        });
        web.setWebViewClient(new WebViewClient() {
            @Override public void onPageFinished(WebView view, String url) {
                if (!done && url.endsWith("location-permission.html")) handler.postDelayed(timeout, 30_000);
            }
        });
        setContentView(web);
        web.loadUrl("file:///android_asset/location-permission.html");
    }
    @Override protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        if (web != null) { web.removeJavascriptInterface("PermissionResult"); web.destroy(); }
        super.onDestroy();
    }
}
