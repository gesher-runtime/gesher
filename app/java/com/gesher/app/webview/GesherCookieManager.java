package com.gesher.app.webview;

import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.webkit.CookieManager;
import android.webkit.ValueCallback;
import android.webkit.WebView;

import java.net.HttpCookie;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * CookieManager over WebView2's cookie manager (docs/webview.md, "Cookies"): the profile
 * every WebView of the process shares. WebView2 answers asynchronously on the WebView
 * thread; getCookie waits for it (up to 5 s), as Android's is synchronous.
 */
final class GesherCookieManager extends CookieManager {
    private static final long WAIT_MS = 5000;
    private volatile boolean mAccept = true;
    private volatile boolean mFileScheme;

    private static Handler callbackHandler(ValueCallback<Boolean> callback) {
        if (callback == null) return null;
        Looper looper = Looper.myLooper();
        if (looper == null) {
            throw new IllegalStateException("Cookie callbacks require a Looper on the calling thread");
        }
        return new Handler(looper);
    }

    private static void post(Handler handler, ValueCallback<Boolean> callback, boolean value) {
        if (handler != null) handler.post(() -> callback.onReceiveValue(value));
    }

    @Override
    public void setAcceptCookie(boolean accept) {
        mAccept = accept;
    }

    @Override
    public boolean acceptCookie() {
        return mAccept;
    }

    @Override
    public void setAcceptThirdPartyCookies(WebView webview, boolean accept) {
        if (webview != null) webview.getSettings().setAcceptThirdPartyCookies(accept);
    }

    @Override
    public boolean acceptThirdPartyCookies(WebView webview) {
        return webview != null && webview.getSettings().getAcceptThirdPartyCookies();
    }

    @Override
    public void setCookie(String url, String value) {
        setCookie(url, value, null);
    }

    @Override
    public void setCookie(String url, String value, ValueCallback<Boolean> callback) {
        // Capture before entering the WebView thread, including validation/failure paths.
        Handler handler = callbackHandler(callback);
        Uri uri = url != null ? Uri.parse(url) : null;
        String host = uri != null ? uri.getHost() : null;
        List<HttpCookie> cookies;
        try {
            cookies = HttpCookie.parse("Set-Cookie: " + value);
        } catch (IllegalArgumentException e) {
            post(handler, callback, false);
            return;
        }
        if (cookies.isEmpty() || host == null) {
            post(handler, callback, false);
            return;
        }
        HttpCookie c = cookies.get(0);
        String domain = c.getDomain() != null ? c.getDomain() : host;
        String path = c.getPath() != null ? c.getPath() : "/";
        double expires = c.getMaxAge() >= 0
                ? System.currentTimeMillis() / 1000.0 + c.getMaxAge() : -1;
        boolean secure = c.getSecure();
        boolean ok = WebView2Host.setCookie(url, c.getName(), c.getValue(), domain, path, expires,
                secure, c.isHttpOnly(), (type, hr, s) -> post(handler, callback, hr >= 0));
        if (!ok) post(handler, callback, false);
    }

    @Override
    public String getCookie(String url) {
        if (url == null || WebView2Host.onWebViewThread()) return null;
        final String[] out = new String[1];
        CountDownLatch done = new CountDownLatch(1);
        if (!WebView2Host.getCookies(url, (type, hr, s) -> {
            out[0] = hr >= 0 && s != null && !s.isEmpty() ? s : null;
            done.countDown();
        })) {
            return null;
        }
        try {
            done.await(WAIT_MS, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return out[0];
    }

    @Override
    public String getCookie(String url, boolean privateBrowsing) {
        return getCookie(url);
    }

    @Override
    public void removeSessionCookie() {
        removeSessionCookies(null);
    }

    @Override
    public void removeSessionCookies(ValueCallback<Boolean> callback) {
        Handler handler = callbackHandler(callback);
        if (!WebView2Host.deleteCookies(2, (type, hr, s) -> post(handler, callback, hr >= 0))) {
            post(handler, callback, false);
        }
    }

    @Override
    public void removeAllCookie() {
        removeAllCookies(null);
    }

    @Override
    public void removeAllCookies(ValueCallback<Boolean> callback) {
        Handler handler = callbackHandler(callback);
        if (!WebView2Host.deleteCookies(1, (type, hr, s) -> post(handler, callback, hr >= 0))) {
            post(handler, callback, false);
        }
    }

    @Override
    public boolean hasCookies() {
        return getCookie("") != null;
    }

    @Override
    public boolean hasCookies(boolean privateBrowsing) {
        return hasCookies();
    }

    @Override
    public void removeExpiredCookie() {
    }

    @Override
    public void flush() {
    }

    @Override
    protected boolean allowFileSchemeCookiesImpl() {
        return mFileScheme;
    }

    @Override
    protected void setAcceptFileSchemeCookiesImpl(boolean accept) {
        mFileScheme = accept;
    }
}
