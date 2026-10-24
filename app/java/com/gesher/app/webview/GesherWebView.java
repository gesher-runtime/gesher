package com.gesher.app.webview;

import android.content.Context;
import android.os.Build;
import android.view.View;
import android.webkit.WebViewFactory;

import com.gesher.app.window.WindowSession;

import java.io.File;
import java.lang.reflect.Field;

/**
 * android.webkit.WebView on Microsoft Edge WebView2 (docs/webview.md, ADR 0041).
 * {@link #install} makes Gesher's WebViewFactoryProvider the process's, so
 * WebViewFactory.getProvider() needs no WebView APK; each WebView is then a WebView2
 * controller in a child window of the app's Win32 window (WebView2Host).
 */
public final class GesherWebView {
    static final String TAG = "gesher-webview";
    /** $GESHER_WEBVIEW_DEBUG: log bounds and a picture of each loaded page. */
    static final boolean DEBUG = System.getenv("GESHER_WEBVIEW_DEBUG") != null;

    private static volatile GesherWebViewFactory sFactory;
    private static final Object sLock = new Object();
    private static boolean sConfigured;
    private static String sUserAgent;

    private GesherWebView() {
    }

    /** Sets WebViewFactory's provider (Main.bootstrap; cheap: nothing loads until used). */
    public static void install() {
        try {
            GesherWebViewFactory factory = new GesherWebViewFactory();
            Field f = WebViewFactory.class.getDeclaredField("sProviderInstance");
            f.setAccessible(true);
            f.set(null, factory);
            sFactory = factory;
        } catch (ReflectiveOperationException | RuntimeException e) {
            log("could not install the WebView provider: " + e);
        }
    }

    static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    /** The user data folder (the app's app_webview, as Android's WebView) and the runtime. */
    static void configure(Context context) {
        synchronized (sLock) {
            if (sConfigured) return;
            sConfigured = true;
            File dir = null;
            try {
                Context app = context.getApplicationContext() != null
                        ? context.getApplicationContext() : context;
                File data = app.getDataDir();
                if (data == null) data = app.getCacheDir();
                if (data != null) dir = new File(data, "app_webview");
            } catch (RuntimeException e) {
                dir = null;
            }
            if (dir == null) dir = new File(System.getProperty("java.io.tmpdir"), "gesher-webview");
            dir.mkdirs();
            String args = System.getenv("GESHER_WEBVIEW_ARGS");
            WebView2Host.configure(dir.getAbsolutePath(), args != null ? args : "");
            String version = WebView2Host.runtimeVersion();
            if (version != null) {
                log("WebView2 Runtime " + version + " (" + WebView2Host.loaderKind()
                        + "), user data in " + dir);
            } else {
                log("WebView2 is unavailable (" + WebView2Host.runtimeError()
                        + "): WebViews stay empty. Install the Microsoft Edge WebView2 Runtime.");
            }
        }
    }

    /** Android WebView's user agent, with WebView2's Chromium version. */
    static String defaultUserAgent() {
        synchronized (sLock) {
            if (sUserAgent == null) {
                String version = WebView2Host.runtimeVersion();
                String major = "130";
                if (version != null) {
                    int dot = version.indexOf('.');
                    major = dot > 0 ? version.substring(0, dot) : version;
                }
                sUserAgent = "Mozilla/5.0 (Linux; Android " + Build.VERSION.RELEASE + "; "
                        + Build.MODEL + " Build/" + Build.ID + "; wv) AppleWebKit/537.36 "
                        + "(KHTML, like Gecko) Version/4.0 Chrome/" + major
                        + ".0.0.0 Mobile Safari/537.36";
            }
            return sUserAgent;
        }
    }

    /** The HWND of the Win32 window the view is attached to, or 0. */
    static long hwndOf(View view) {
        WindowSession session = WindowSession.get();
        if (session == null || !view.isAttachedToWindow()) return 0;
        return session.hwndOf(view.getWindowToken());
    }
}
