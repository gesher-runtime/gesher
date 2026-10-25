package com.gesher.app.webview;

import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/**
 * One WebView2 controller (compat/webview/webview_host.h): the natives of
 * libgesher_webview.so, a builtin library of dalvikvm (compat/webview/webview_jni.cc).
 * Every method posts its work to the process's WebView thread and returns; what WebView2
 * reports comes back on that thread through {@link Client} (docs/webview.md).
 */
public final class WebView2Host {
    /** Event types, as webview_host.h's {@code Event}. */
    public static final int EVENT_READY = 1;
    public static final int EVENT_FAILED = 2;
    public static final int EVENT_PAGE_STARTED = 3;
    public static final int EVENT_PAGE_FINISHED = 4;
    public static final int EVENT_SOURCE_CHANGED = 5;
    public static final int EVENT_HISTORY = 6;
    public static final int EVENT_TITLE = 7;
    public static final int EVENT_CONTENT_LOADING = 8;
    public static final int EVENT_DOM_CONTENT_LOADED = 9;
    public static final int EVENT_SCRIPT_RESULT = 10;
    public static final int EVENT_DIALOG = 11;
    public static final int EVENT_CONSOLE = 12;
    public static final int EVENT_DOWNLOAD = 13;
    public static final int EVENT_FOCUS = 14;
    public static final int EVENT_PROCESS_FAILED = 15;
    public static final int EVENT_NEW_WINDOW = 16;
    public static final int EVENT_CAPTURE = 17;
    public static final int EVENT_PARKED = 18;

    /** {@link #run} commands, as {@code Host::Command}. */
    public static final int RELOAD = 1;
    public static final int STOP = 2;
    public static final int GO_BACK = 3;
    public static final int GO_FORWARD = 4;
    public static final int FOCUS = 5;
    public static final int BLUR = 6;
    public static final int DEVTOOLS = 7;

    /** COREWEBVIEW2_BROWSING_DATA_KINDS. */
    public static final int DATA_FILE_SYSTEMS = 0x1;
    public static final int DATA_INDEXED_DB = 0x2;
    public static final int DATA_LOCAL_STORAGE = 0x4;
    public static final int DATA_WEB_SQL = 0x8;
    public static final int DATA_CACHE_STORAGE = 0x10;
    public static final int DATA_ALL_DOM_STORAGE = 0x20;
    public static final int DATA_COOKIES = 0x40;
    public static final int DATA_DISK_CACHE = 0x100;

    /** What a host reports, on the WebView thread. */
    public interface Client {
        void onEvent(int type, long id, int arg, String a, String b, String c, byte[] bytes);

        /** True cancels the navigation (shouldOverrideUrlLoading). Synchronous. */
        boolean onNavigationStarting(String url, boolean userInitiated, boolean redirect);

        /** Answer with {@link #respond}, from any thread. */
        void onResourceRequest(long id, String url, String method, String headers, int context,
                boolean mainFrame);

        /** {"v": value} or {"e": message}. Synchronous. */
        String onBridgeCall(String object, String method, String argsJson);
    }

    private static final Object sLock = new Object();
    private static Boolean sLoaded;
    private static String sLoadError;

    /** Loads libgesher_webview.so once; false (and {@link #loadError}) when it is missing. */
    public static boolean load() {
        synchronized (sLock) {
            if (sLoaded == null) {
                try {
                    System.loadLibrary("gesher_webview");
                    sLoaded = true;
                } catch (UnsatisfiedLinkError e) {
                    sLoaded = false;
                    sLoadError = String.valueOf(e.getMessage());
                }
            }
            return sLoaded;
        }
    }

    public static String loadError() {
        return sLoadError;
    }

    /** The installed WebView2 Runtime's version, or null (then {@link #runtimeError}). */
    public static String runtimeVersion() {
        if (!load()) return null;
        String v = nRuntimeVersion();
        return v == null || v.startsWith("!") ? null : v;
    }

    /** Why there is no runtime ("0x80070002"), or the library's load error. */
    public static String runtimeError() {
        if (!load()) return "libgesher_webview.so: " + sLoadError;
        String v = nRuntimeVersion();
        return v != null && v.startsWith("!") ? v.substring(1) : null;
    }

    public static String loaderKind() {
        return load() ? nLoaderKind() : "none";
    }

    /** The process's user data folder and browser arguments (the first call wins). */
    public static void configure(String userDataFolder, String browserArguments) {
        if (load()) nConfigure(userDataFolder, browserArguments);
    }

    private final Client mClient;
    private volatile long mHandle;

    private WebView2Host(Client client) {
        mClient = client;
    }

    /** A new host, parked (hidden) until {@link #setBounds} gives it a parent window. */
    public static WebView2Host create(Client client, double scale, int background) {
        if (!load()) return null;
        WebView2Host host = new WebView2Host(client);
        host.mHandle = nCreate(host, 0, scale, background);
        return host.mHandle != 0 ? host : null;
    }

    public boolean isDestroyed() {
        return mHandle == 0;
    }

    public void destroy() {
        long h = mHandle;
        mHandle = 0;
        if (h != 0) nDestroy(h);
    }

    public void navigate(String url, String headers) {
        long h = mHandle;
        if (h != 0) nNavigate(h, url, headers);
    }

    public void navigateToString(String html) {
        long h = mHandle;
        if (h != 0) nNavigateToString(h, html);
    }

    public void run(int command) {
        long h = mHandle;
        if (h != 0) nRun(h, command);
    }

    /**
     * {@code holes}: left, top, right, bottom of each of the app's views drawn over the
     * WebView, in the window's coordinates (cut out of the WebView2 window), or null.
     */
    public void setBounds(long parentHwnd, int x, int y, int width, int height, int clipX,
            int clipY, int clipWidth, int clipHeight, boolean visible, int[] holes) {
        long h = mHandle;
        if (h != 0) {
            nSetBounds(h, parentHwnd, x, y, width, height, clipX, clipY, clipWidth, clipHeight,
                    visible, holes);
        }
    }

    public void park(long id) {
        long h = mHandle;
        if (h != 0) nPark(h, id);
    }

    public void setSettings(boolean javascript, boolean zoomControls, boolean pinchZoom,
            boolean devtools, boolean interceptAll, String userAgent) {
        long h = mHandle;
        if (h != 0) {
            nSetSettings(h, javascript, zoomControls, pinchZoom, devtools, interceptAll,
                    userAgent);
        }
    }

    public void setZoom(double factor) {
        long h = mHandle;
        if (h != 0) nSetZoom(h, factor);
    }

    public void setBackground(int argb) {
        long h = mHandle;
        if (h != 0) nSetBackground(h, argb);
    }

    public void executeScript(String script, long callbackId) {
        long h = mHandle;
        if (h != 0) nExecuteScript(h, script, callbackId);
    }

    public void setDocumentScript(String script) {
        long h = mHandle;
        if (h != 0) nSetDocumentScript(h, script);
    }

    /** status 0: the request goes to the network as it is. */
    public void respond(long id, int status, String reason, String headers, byte[] body) {
        long h = mHandle;
        if (h != 0) nRespond(h, id, status, reason, headers, body);
    }

    public void completeDialog(long id, boolean accept, String text) {
        long h = mHandle;
        if (h != 0) nCompleteDialog(h, id, accept, text);
    }

    public void capturePreview(long callbackId) {
        long h = mHandle;
        if (h != 0) nCapturePreview(h, callbackId);
    }

    // ---- Cookies and browsing data (process-wide) ----

    /** Global results: kGlobalCookies (1) and kGlobalDone (2). */
    public interface GlobalCallback {
        void onResult(int type, int hresult, String value);
    }

    private static final AtomicLong sNextGlobal = new AtomicLong(1);
    private static final ConcurrentHashMap<Long, GlobalCallback> sGlobal =
            new ConcurrentHashMap<>();

    private static long register(GlobalCallback callback) {
        long id = sNextGlobal.getAndIncrement();
        if (callback != null) sGlobal.put(id, callback);
        return id;
    }

    /** Returns false when WebView2 is unavailable (the callback is not called). */
    public static boolean setCookie(String url, String name, String value, String domain,
            String path, double expires, boolean secure, boolean httpOnly,
            GlobalCallback callback) {
        if (!load()) return false;
        nSetCookie(url, name, value, domain, path, expires, secure, httpOnly,
                register(callback));
        return true;
    }

    public static boolean getCookies(String url, GlobalCallback callback) {
        if (!load()) return false;
        nGetCookies(url, register(callback));
        return true;
    }

    /** which: 1 all, 2 session cookies. */
    public static boolean deleteCookies(int which, GlobalCallback callback) {
        if (!load()) return false;
        nDeleteCookies(which, register(callback));
        return true;
    }

    public static boolean clearBrowsingData(int kinds, GlobalCallback callback) {
        if (!load()) return false;
        nClearBrowsingData(kinds, register(callback));
        return true;
    }

    // ---- Called by the natives, on the WebView thread ----

    private void onEvent(int type, long id, int arg, String a, String b, String c, byte[] bytes) {
        mClient.onEvent(type, id, arg, a, b, c, bytes);
    }

    private boolean onNavigationStarting(String url, boolean user, boolean redirect) {
        return mClient.onNavigationStarting(url, user, redirect);
    }

    private void onResourceRequest(long id, String url, String method, String headers,
            int context, boolean mainFrame) {
        mClient.onResourceRequest(id, url, method, headers, context, mainFrame);
    }

    private String onBridgeCall(String object, String method, String args) {
        return mClient.onBridgeCall(object, method, args);
    }

    private static void onGlobal(int type, long id, int arg, String a) {
        GlobalCallback cb = sGlobal.remove(id);
        if (cb != null) cb.onResult(type, arg, a);
    }

    /** Whether the calling thread is the WebView thread (which must not wait for itself). */
    public static boolean onWebViewThread() {
        return "WebView2".equals(Thread.currentThread().getName());
    }

    private static native String nRuntimeVersion();
    private static native String nLoaderKind();
    private static native void nConfigure(String userData, String args);
    private static native long nCreate(WebView2Host peer, long parent, double scale,
            int background);
    private static native void nDestroy(long h);
    private static native void nNavigate(long h, String url, String headers);
    private static native void nNavigateToString(long h, String html);
    private static native void nRun(long h, int command);
    private static native void nSetBounds(long h, long parent, int x, int y, int width,
            int height, int clipX, int clipY, int clipWidth, int clipHeight, boolean visible,
            int[] holes);
    private static native void nPark(long h, long id);
    private static native void nSetSettings(long h, boolean javascript, boolean zoomControls,
            boolean pinchZoom, boolean devtools, boolean interceptAll, String userAgent);
    private static native void nSetZoom(long h, double factor);
    private static native void nSetBackground(long h, int argb);
    private static native void nExecuteScript(long h, String script, long callbackId);
    private static native void nSetDocumentScript(long h, String script);
    private static native void nRespond(long h, long id, int status, String reason,
            String headers, byte[] body);
    private static native void nCompleteDialog(long h, long id, boolean accept, String text);
    private static native void nCapturePreview(long h, long callbackId);
    private static native void nSetCookie(String url, String name, String value, String domain,
            String path, double expires, boolean secure, boolean httpOnly, long callbackId);
    private static native void nGetCookies(String url, long callbackId);
    private static native void nDeleteCookies(int which, long callbackId);
    private static native void nClearBrowsingData(int kinds, long callbackId);
}
