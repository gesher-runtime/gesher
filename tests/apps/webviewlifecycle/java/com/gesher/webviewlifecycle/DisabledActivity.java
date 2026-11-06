/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.webviewlifecycle;

import android.app.Activity;
import android.os.Bundle;
import android.webkit.CookieManager;
import android.webkit.ServiceWorkerController;
import android.webkit.WebSettings;
import android.webkit.WebStorage;
import android.webkit.WebView;

public final class DisabledActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Checks.report(this,"disable-before-use",()->{
            Checks.require(LifecycleApplication.providerInitiallyNull && LifecycleApplication.disabledBeforeUse);
            Checks.require(LifecycleApplication.setupFailure==null && Checks.provider()==null);
            WebView.disableWebView();
            Checks.installAgain(); Checks.installAgain();
            Checks.disabled(()->CookieManager.getInstance());
            Checks.disabled(()->WebSettings.getDefaultUserAgent(this));
            Checks.disabled(()->new WebView(this));
            Checks.disabled(()->WebStorage.getInstance());
            Checks.disabled(()->ServiceWorkerController.getInstance());
            Checks.require(Checks.provider()==null);
            Checks.require(!new java.io.File(getDataDir(),"app_webview").exists());
        });
    }
}
