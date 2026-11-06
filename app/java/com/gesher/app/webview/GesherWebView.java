package com.gesher.app.webview;

import android.content.Context;
import android.os.Build;
import android.view.View;
import android.webkit.WebViewFactory;
import android.webkit.WebViewFactoryProvider;

import com.gesher.app.window.WindowSession;

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.function.Supplier;

/**
 * android.webkit.WebView on Microsoft Edge WebView2 (docs/webview.md, ADR 0041).
 * {@link #install} registers Gesher's lazy WebViewFactoryProvider, so
 * WebViewFactory.getProvider() needs no WebView APK; each WebView is then a WebView2
 * controller in a child window of the app's Win32 window (WebView2Host).
 */
public final class GesherWebView {
    static final String TAG = "gesher-webview";
    /** $GESHER_WEBVIEW_DEBUG: log bounds and a picture of each loaded page. */
    static final boolean DEBUG = System.getenv("GESHER_WEBVIEW_DEBUG") != null;

    private static final Supplier<WebViewFactoryProvider> sFactory = GesherWebViewFactory::new;
    private static final Object sLock = new Object();
    private static boolean sConfigured;
    private static String sUserAgent;

    private GesherWebView() {
    }

    /** Registers the provider without initializing WebView before Application.onCreate. */
    public static void install() {
        try {
            Field lock = WebViewFactory.class.getDeclaredField("sProviderLock");
            Field factory = WebViewFactory.class.getDeclaredField("sProviderFactory");
            Field instance = WebViewFactory.class.getDeclaredField("sProviderInstance");
            lock.setAccessible(true);
            factory.setAccessible(true);
            instance.setAccessible(true);
            synchronized (lock.get(null)) {
                Object registered = factory.get(null);
                if (registered == sFactory) return;
                if (registered != null || instance.get(null) != null) {
                    throw new IllegalStateException("WebView provider already registered or initialized");
                }
                factory.set(null, sFactory);
            }
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("The framework must support lazy WebView providers", e);
        }
    }

    private static String dataDirectorySuffix() {
        try {
            Method getter = WebViewFactory.class.getDeclaredMethod("getDataDirectorySuffix");
            getter.setAccessible(true);
            return (String) getter.invoke(null);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Cannot read the WebView data directory suffix", e);
        }
    }

    static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    /** The user data folder (the app's app_webview, as Android's WebView) and the runtime. */
    static void configure(Context context) {
        // Read under WebViewFactory's lock before taking this class's configuration lock.
        String suffix = dataDirectorySuffix();
        String postfix = suffix != null ? "_" + suffix : "";
        synchronized (sLock) {
            if (sConfigured) return;
            File dir = null;
            File parent = null;
            try {
                Context app = context.getApplicationContext() != null
                        ? context.getApplicationContext() : context;
                File data = app.getDataDir();
                if (data == null) data = app.getCacheDir();
                if (data != null) {
                    parent = data;
                    dir = new File(data, "app_webview" + postfix);
                }
            } catch (RuntimeException e) {
                dir = null;
            }
            if (dir == null) {
                parent = new File(System.getProperty("java.io.tmpdir"));
                dir = new File(parent, "gesher-webview" + postfix);
            }
            if (!dir.mkdirs() && !dir.isDirectory()) {
                throw new IllegalStateException("Cannot create the WebView data directory");
            }
            try {
                File canonical = dir.getCanonicalFile();
                if (!parent.getCanonicalFile().equals(canonical.getParentFile())) {
                    throw new IllegalArgumentException("WebView data directory must stay inside its parent");
                }
                if (suffix != null && !dir.getName().equals(canonical.getName())) {
                    throw new IllegalArgumentException("WebView suffix aliases another directory name");
                }
            } catch (IOException e) {
                throw new IllegalStateException("Cannot resolve the WebView data directory", e);
            }
            String args = System.getenv("GESHER_WEBVIEW_ARGS");
            WebView2Host.configure(dir.getAbsolutePath(), args != null ? args : "");
            sConfigured = true;
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

    /**
     * The HWND of a WebView's WebView2 window (its container), or 0: UI Automation shows
     * WebView2's own tree there (accessibility, ADR 0058).
     */
    public static long containerHwnd(android.webkit.WebView view) {
        Object p = view != null ? view.getWebViewProvider() : null;
        if (!(p instanceof GesherWebViewProvider)) return 0;
        WebView2Host host = ((GesherWebViewProvider) p).host();
        return host != null ? host.containerHwnd() : 0;
    }

    /** The HWND of the Win32 window the view is attached to, or 0. */
    static long hwndOf(View view) {
        WindowSession session = WindowSession.get();
        if (session == null || !view.isAttachedToWindow()) return 0;
        return session.hwndOf(view.getWindowToken());
    }
}
