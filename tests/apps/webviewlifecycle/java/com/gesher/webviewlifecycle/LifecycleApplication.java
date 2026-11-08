/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.webviewlifecycle;

import android.app.Application;
import android.webkit.WebView;

/** Runs the public startup APIs at their legitimate Application.onCreate lifecycle point. */
public final class LifecycleApplication extends Application {
    static boolean providerInitiallyNull;
    static boolean disabledBeforeUse;
    static String setupFailure;
    @Override public void onCreate() {
        super.onCreate();
        try {
            providerInitiallyNull = Checks.provider() == null;
            String process = Application.getProcessName();
            if (process.endsWith(":suffixA")) WebView.setDataDirectorySuffix("fixtureA");
            else if (process.endsWith(":suffixB")) WebView.setDataDirectorySuffix("fixtureB");
            else if (process.endsWith(":suffixAlias")) WebView.setDataDirectorySuffix("FIXTUREa");
            else if (!process.contains(":")) {
                try {
                    WebView.disableWebView();
                    disabledBeforeUse = true;
                } catch (IllegalStateException expectedOnOldBuild) {
                    disabledBeforeUse = false;
                }
                System.out.println("viewlife: application disableBeforeUse=" + disabledBeforeUse
                        + " providerInitiallyNull=" + providerInitiallyNull);
            }
        } catch (Throwable error) {
            setupFailure = error.getClass().getSimpleName();
            System.out.println("viewlife: application setupFailure=" + setupFailure);
        }
    }
}
