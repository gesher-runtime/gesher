/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.webviewlifecycle;

import android.app.Activity;
import android.webkit.CookieManager;
import android.webkit.WebView;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

final class Checks {
    interface Operation { void run() throws Exception; }
    static void require(boolean value) { if (!value) throw new AssertionError("Lifecycle contract failed"); }
    static Object provider() throws Exception {
        Field field=Class.forName("android.webkit.WebViewFactory").getDeclaredField("sProviderInstance");
        field.setAccessible(true);
        return field.get(null);
    }
    static String suffix() throws Exception {
        Method method=Class.forName("android.webkit.WebViewFactory").getDeclaredMethod("getDataDirectorySuffix");
        method.setAccessible(true);
        return (String)method.invoke(null);
    }
    static void installAgain() throws Exception {
        Class.forName("com.gesher.app.webview.GesherWebView").getMethod("install").invoke(null);
    }
    static void configure(Activity activity) throws Exception {
        Object version=Class.forName("com.gesher.app.webview.WebView2Host").getMethod("runtimeVersion").invoke(null);
        require(version instanceof String && !((String)version).isEmpty());
        Method method=Class.forName("com.gesher.app.webview.GesherWebView").getDeclaredMethod("configure",android.content.Context.class);
        method.setAccessible(true);
        method.invoke(null,activity);
    }
    static void disabled(Operation action) throws Exception {
        try { action.run(); throw new AssertionError("Disabled WebView API succeeded"); }
        catch (IllegalStateException expected) {
            require(expected.getMessage()!=null && expected.getMessage().contains("disabled"));
        }
    }
    static void disableAfterUse() {
        try { WebView.disableWebView(); throw new AssertionError("Used WebView was disabled"); }
        catch (IllegalStateException expected) { require(expected.getMessage().contains("initialized")); }
    }
    static void initialized() throws Exception {
        require(LifecycleApplication.providerInitiallyNull);
        require(LifecycleApplication.setupFailure==null);
        CookieManager first=CookieManager.getInstance();
        require(first!=null && provider()!=null && first==CookieManager.getInstance());
    }
    static void report(Activity activity,String name,Operation action) {
        try { action.run(); System.out.println("viewlife: PASS "+name); }
        catch (Throwable error) {
            System.out.println("viewlife: FAIL "+name+" kind="+error.getClass().getSimpleName());
            error.printStackTrace(System.out);
        }
        finally { activity.finish(); }
    }
}
