package com.gesher.app.webview;

import android.content.Context;
import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Picture;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.net.http.SslCertificate;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.print.PrintDocumentAdapter;
import android.util.Base64;
import android.util.Log;
import android.view.DragEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.net.http.SslError;
import android.webkit.ClientCertRequest;
import android.webkit.ConsoleMessage;
import android.webkit.DownloadListener;
import android.webkit.GeolocationPermissions;
import android.webkit.HttpAuthHandler;
import android.webkit.JsDialogHelper;
import android.webkit.PermissionRequest;
import android.webkit.SslErrorHandler;
import android.webkit.WebStorage;
import android.webkit.JsPromptResult;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.ValueCallback;
import android.webkit.WebBackForwardList;
import android.webkit.WebChromeClient;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.WebViewProvider;
import android.webkit.WebViewRenderProcess;
import android.webkit.WebViewRenderProcessClient;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedWriter;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.security.Principal;
import java.security.PrivateKey;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/**
 * One android.webkit.WebView on WebView2 (docs/webview.md, ADR 0041): its
 * WebViewProvider, ViewDelegate and ScrollDelegate. The WebView2 controller
 * (WebView2Host) lives in a child window of the app's Win32 window, kept at the view's
 * visible bounds (a ViewTreeObserver's pre-draw, scroll and layout callbacks); what
 * WebView2 reports reaches the app's WebViewClient and WebChromeClient on the WebView's
 * Looper.
 */
final class GesherWebViewProvider implements WebViewProvider, WebViewProvider.ViewDelegate,
        WebViewProvider.ScrollDelegate, WebView2Host.Client {
    private static final String TAG = GesherWebView.TAG;
    /** WebView.setWebContentsDebuggingEnabled. */
    static volatile boolean sDevTools;
    /** The process's first page load is logged (tests/e2e: expectLog). */
    private static boolean sFirstPage;
    private static final ExecutorService sRequests = Executors.newCachedThreadPool(r -> {
        Thread t = new Thread(r, "WebView requests");
        t.setDaemon(true);
        return t;
    });
    private static final long WAIT_FOR_UI_MS = 5000;

    private final WebView mView;
    private final WebView.PrivateAccess mAccess;
    private final Context mContext;
    private final Handler mUi;
    private final GesherWebSettings mSettings;
    private WebView2Host mHost;
    private boolean mDestroyed;

    private volatile WebViewClient mClient = new WebViewClient();
    private volatile WebChromeClient mChrome;
    private volatile DownloadListener mDownloadListener;
    private volatile boolean mOverridesNavigation;
    private volatile boolean mInterceptsRequests;
    private final Map<String, Object> mInterfaces = new ConcurrentHashMap<>();
    private final boolean mRequireAnnotation;

    // What WebView2 reported (the WebView's Looper).
    private String mUrl;
    private String mOriginalUrl;
    private String mTitle;
    private int mProgress = 100;
    private boolean mCanGoBack;
    private boolean mCanGoForward;
    private int mContentHeight;
    private boolean mPaused;

    private final AtomicLong mNextCallback = new AtomicLong(1);
    private final Map<Long, ValueCallback<String>> mScriptCallbacks = new ConcurrentHashMap<>();
    /** DevTools protocol results (the result's JSON, or null on an error). */
    private final Map<Long, ValueCallback<String>> mDevToolsCallbacks = new ConcurrentHashMap<>();
    /** Results a thread waits for (capturePicture, findAll): answered on the WebView thread. */
    private final Map<Long, Sync> mSync = new ConcurrentHashMap<>();
    /** The ends of WebMessagePorts sent to the page, by id. */
    private final Map<String, GesherWebMessagePort> mPagePorts = new ConcurrentHashMap<>();
    private volatile GesherBackForwardList mHistory = GesherBackForwardList.EMPTY;
    private WebView.FindListener mFindListener;
    /** WebView2's Find API is there (else PageScripts.find). */
    private boolean mNativeFind = true;
    private String mFindTerm;
    /** Windows file dialogs open for the page's file choosers (their Windows paths), by id. */
    private final Map<Long, ValueCallback<String[]>> mFileDialogs = new ConcurrentHashMap<>();
    private static final Map<String, String[]> sHttpAuth = new ConcurrentHashMap<>();

    private static final class Sync {
        final CountDownLatch done = new CountDownLatch(1);
        volatile int hresult;
        volatile String text;
        volatile byte[] bytes;
    }

    /** androidx.webkit's WebMessageListeners (ADR 0058), by JavaScript object name. */
    private final Map<String, MessageListener> mMessageListeners =
            Collections.synchronizedMap(new LinkedHashMap<>());
    /** androidx.webkit's document-start scripts, in the order they were added. */
    private final List<StartScript> mStartScripts =
            new java.util.concurrent.CopyOnWriteArrayList<>();
    /** navigator.geolocation from Gesher's LocationManager (ADR 0058). */
    private final GeolocationShim mGeolocation = new GeolocationShim(this);

    static final class MessageListener {
        final String name;
        final String[] rules;
        final java.lang.reflect.InvocationHandler listener;
        final Map<String, Object> replyProxies = new ConcurrentHashMap<>();

        MessageListener(String name, String[] rules, java.lang.reflect.InvocationHandler listener) {
            this.name = name;
            this.rules = rules;
            this.listener = listener;
        }
    }

    static final class StartScript {
        final String script;
        final String[] rules;

        StartScript(String script, String[] rules) {
            this.script = script;
            this.rules = rules;
        }
    }

    // ---- For androidx.webkit's glue (SupportLib) and GeolocationShim ----

    WebView view() {
        return mView;
    }

    Context context() {
        return mContext;
    }

    WebView2Host host() {
        return mHost;
    }

    WebChromeClient chrome() {
        return mChrome;
    }

    GesherWebSettings settings() {
        return mSettings;
    }

    String currentUrl() {
        return mUrl;
    }

    Handler uiHandler() {
        return mUi;
    }

    /** The origin ("scheme://host[:port]/") the app sees for a WebView2 URL. */
    static String originOf(String webView2Url) {
        return appOrigin(webView2Url);
    }

    void addMessageListener(String name, String[] rules,
            java.lang.reflect.InvocationHandler listener) {
        if (name == null || name.isEmpty() || listener == null) {
            throw new IllegalArgumentException("a JavaScript object name and a listener are needed");
        }
        if (!name.matches("[A-Za-z_$][A-Za-z0-9_$]*")) {
            throw new IllegalArgumentException("not a JavaScript identifier: " + name);
        }
        String[] r = checkRules(rules);
        synchronized (mMessageListeners) {
            if (mMessageListeners.containsKey(name)) {
                throw new IllegalArgumentException(name + " is already added");
            }
            mMessageListeners.put(name, new MessageListener(name, r, listener));
        }
        runOnUi(this::updateDocumentScript);
    }

    void removeMessageListener(String name) {
        if (mMessageListeners.remove(name) != null) runOnUi(this::updateDocumentScript);
    }

    StartScript addStartScript(String script, String[] rules) {
        if (script == null) throw new IllegalArgumentException("null script");
        StartScript s = new StartScript(script, checkRules(rules));
        mStartScripts.add(s);
        runOnUi(this::updateDocumentScript);
        return s;
    }

    void removeStartScript(StartScript s) {
        if (mStartScripts.remove(s)) runOnUi(this::updateDocumentScript);
    }

    /** A JavaScriptReplyProxy's message to the page's object {@code name}. */
    void postToListener(String name, String data) {
        postToListener(name, new WebMessage(data));
    }

    void postToListener(String name, WebMessage message) {
        WebView2Host host = mHost;
        if (host == null) return;
        try {
            JSONObject m = new JSONObject();
            m.put("gesher", "listener");
            m.put("name", name);
            WebMessagePayload.write(m, message);
            host.postWebMessage(m.toString());
        } catch (org.json.JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    private void runOnUi(Runnable r) {
        if (Looper.myLooper() == mUi.getLooper()) r.run(); else mUi.post(r);
    }

    /** Android's allowed origin rules, checked as AwContents checks them. */
    private static String[] checkRules(String[] rules) {
        if (rules == null) throw new IllegalArgumentException("null allowedOriginRules");
        for (String r : rules) {
            if (r == null || !(r.equals("*") || r.matches(
                    "[a-zA-Z][a-zA-Z0-9+.-]*://(\\*\\.)?[^:/*]*(:\\d+)?/?"))) {
                throw new IllegalArgumentException("not an origin rule: " + r);
            }
        }
        return rules.clone();
    }

    /** loadDataWithBaseURL: the data the next main document at this URL gets. */
    private volatile String mDataUrl;
    private volatile byte[] mData;
    private volatile String mDataMime;

    // Bounds (the WebView's Looper).
    private boolean mAttached;
    private final long[] mPlaced = new long[10];
    private boolean mPlacedOnce;
    private final int[] mLocation = new int[2];
    private final Rect mClip = new Rect();
    private final Rect mHoleRect = new Rect();
    private int[] mHoles = new int[0];
    private int mHoleCount;
    private int[] mPlacedHoles = new int[0];
    private volatile CountDownLatch mParked;

    private final ViewTreeObserver.OnPreDrawListener mPreDraw = () -> {
        updateBounds();
        return true;
    };
    private final ViewTreeObserver.OnScrollChangedListener mScrolled = this::updateBounds;
    private final ViewTreeObserver.OnGlobalLayoutListener mLaidOut = this::updateBounds;

    GesherWebViewProvider(WebView view, WebView.PrivateAccess access) {
        mView = view;
        mAccess = access;
        mContext = view.getContext();
        Looper looper = Looper.myLooper() != null ? Looper.myLooper() : Looper.getMainLooper();
        mUi = new Handler(looper);
        mRequireAnnotation = mContext.getApplicationInfo().targetSdkVersion >= 17;
        GesherWebView.configure(mContext);
        mSettings = new GesherWebSettings(GesherWebView.defaultUserAgent(), this::settingsChanged);
    }

    // ---- WebViewProvider ----

    @Override
    public void init(Map<String, Object> javaScriptInterfaces, boolean privateBrowsing) {
        if (javaScriptInterfaces != null) {
            for (Map.Entry<String, Object> e : javaScriptInterfaces.entrySet()) {
                if (e.getKey() != null && e.getValue() != null) {
                    mInterfaces.put(e.getKey(), e.getValue());
                }
            }
        }
        float density = mContext.getResources().getDisplayMetrics().density;
        mHost = WebView2Host.create(this, density > 0 ? density : 1, 0xffffffff);
        if (mHost == null) {
            GesherWebView.log("no WebView2 for this WebView: " + WebView2Host.runtimeError());
            return;
        }
        applySettings();
        updateDocumentScript();
        // <input type=file>: WebChromeClient.onShowFileChooser, else a Windows file dialog.
        mHost.listenDevTools("Page.fileChooserOpened");
        mHost.callDevTools("Page.enable", "{}", 0);
        mHost.callDevTools("Page.setInterceptFileChooserDialog", "{\"enabled\":true}", 0);
    }

    private void settingsChanged() {
        if (Looper.myLooper() == mUi.getLooper()) {
            applySettings();
        } else {
            mUi.post(this::applySettings);
        }
    }

    private void applySettings() {
        if (mHost == null) return;
        WebSettings s = mSettings;
        boolean zoom = s.supportZoom() && s.getBuiltInZoomControls();
        boolean intercept = mInterceptsRequests || s.getBlockNetworkLoads()
                || s.getBlockNetworkImage() || !s.getLoadsImagesAutomatically();
        mHost.setSettings(s.getJavaScriptEnabled(), zoom, zoom, sDevTools, intercept,
                s.getSafeBrowsingEnabled(), s.getUserAgentString());
        // Text zoom scales the text alone (PageScripts.textZoom), not the page.
        int textZoom = s.getTextZoom() > 0 ? s.getTextZoom() : 100;
        if (textZoom != mTextZoom) {
            mTextZoom = textZoom;
            evaluateJavaScript(PageScripts.textZoom(textZoom), null);
        }
        updateDocumentScript();
    }

    private int mTextZoom = 100;

    private String mLastDocumentScript;

    private void updateDocumentScript() {
        if (mHost == null) return;
        StringBuilder script = new StringBuilder();
        if (!mSettings.getDomStorageEnabled()) {
            // DOM storage off (Android's default): no localStorage or sessionStorage.
            script.append("try { Object.defineProperty(window, 'localStorage', {value: null});"
                    + " Object.defineProperty(window, 'sessionStorage', {value: null}); }"
                    + " catch (e) {}\n");
        }
        script.append(PageScripts.PORTS);
        if (mTextZoom != 100) script.append(PageScripts.textZoom(mTextZoom));
        script.append(JsBridge.script(mInterfaces, mRequireAnnotation));
        // androidx.webkit (ADR 0058): WebMessageListeners' objects, then the app's
        // document-start scripts, in the order they were added.
        script.append(PageScripts.ORIGINS);
        script.append(GeolocationShim.SCRIPT);
        for (MessageListener l : mMessageListeners.values()) {
            script.append(PageScripts.messageListener(l.name, l.rules));
        }
        for (StartScript s : mStartScripts) {
            script.append(PageScripts.documentStartScript(s.script, s.rules));
        }
        String s = script.toString();
        if (s.equals(mLastDocumentScript)) return;
        mLastDocumentScript = s;
        mHost.setDocumentScript(s);
    }

    @Override
    public void setHorizontalScrollbarOverlay(boolean overlay) {
    }

    @Override
    public void setVerticalScrollbarOverlay(boolean overlay) {
    }

    @Override
    public boolean overlayHorizontalScrollbar() {
        return true;
    }

    @Override
    public boolean overlayVerticalScrollbar() {
        return false;
    }

    @Override
    public int getVisibleTitleHeight() {
        return 0;
    }

    @Override
    public SslCertificate getCertificate() {
        return null;
    }

    @Override
    public void setCertificate(SslCertificate certificate) {
    }

    @Override
    public void savePassword(String host, String username, String password) {
    }

    @Override
    public void setHttpAuthUsernamePassword(String host, String realm, String username,
            String password) {
        if (host != null && realm != null) {
            sHttpAuth.put(host + "\n" + realm, new String[] {username, password});
        }
    }

    @Override
    public String[] getHttpAuthUsernamePassword(String host, String realm) {
        return host != null && realm != null ? sHttpAuth.get(host + "\n" + realm) : null;
    }

    @Override
    public void destroy() {
        if (mDestroyed) return;
        mDestroyed = true;
        removeListeners();
        mGeolocation.pageGone();
        if (mHost != null) mHost.destroy();
        mHost = null;
    }

    @Override
    public void setNetworkAvailable(boolean networkUp) {
    }

    @Override
    public WebBackForwardList saveState(Bundle outState) {
        if (outState != null && mUrl != null) outState.putString("gesher.webview.url", mUrl);
        return null;
    }

    @Override
    public boolean savePicture(Bundle b, File dest) {
        return false;
    }

    @Override
    public boolean restorePicture(Bundle b, File src) {
        return false;
    }

    @Override
    public WebBackForwardList restoreState(Bundle inState) {
        String url = inState != null ? inState.getString("gesher.webview.url") : null;
        if (url != null) loadUrl(url);
        return null;
    }

    @Override
    public void loadUrl(String url, Map<String, String> additionalHttpHeaders) {
        if (url == null) return;
        if (url.startsWith("javascript:")) {
            evaluateJavaScript(Uri.decode(url.substring("javascript:".length())), null);
            return;
        }
        StringBuilder headers = new StringBuilder();
        if (additionalHttpHeaders != null) {
            for (Map.Entry<String, String> h : additionalHttpHeaders.entrySet()) {
                headers.append(h.getKey()).append(": ").append(h.getValue()).append("\r\n");
            }
        }
        mOriginalUrl = url;
        navigate(FileUrls.toWebView2(url), headers.toString());
    }

    @Override
    public void loadUrl(String url) {
        loadUrl(url, null);
    }

    private void navigate(String webView2Url, String headers) {
        if (mHost == null) {
            GesherWebView.log("loading " + webView2Url + ": WebView2 is unavailable");
            return;
        }
        mHost.navigate(webView2Url, headers);
    }

    @Override
    public void postUrl(String url, byte[] postData) {
        if (url == null) return;
        if (!url.startsWith("http:") && !url.startsWith("https:")) {
            // As Android: other URLs load without the data.
            loadUrl(url);
            return;
        }
        mOriginalUrl = url;
        if (mHost == null) {
            GesherWebView.log("posting to " + url + ": WebView2 is unavailable");
            return;
        }
        // NavigateWithWebResourceRequest with the body, form-encoded as Android posts it.
        mHost.navigate(url, "Content-Type: application/x-www-form-urlencoded\r\n", "POST",
                postData != null ? postData : new byte[0]);
    }

    @Override
    public void loadData(String data, String mimeType, String encoding) {
        if (data == null) data = "";
        if (mimeType == null || mimeType.isEmpty()) mimeType = "text/html";
        String base64;
        if ("base64".equalsIgnoreCase(encoding)) {
            base64 = data;
        } else {
            // The data of a data: URL: percent escapes are decoded.
            String decoded;
            try {
                decoded = URLDecoder.decode(data.replace("+", "%2B"), "UTF-8");
            } catch (Exception e) {
                decoded = data;
            }
            base64 = Base64.encodeToString(decoded.getBytes(StandardCharsets.UTF_8),
                    Base64.NO_WRAP);
        }
        String url = "data:" + mimeType + ";charset=utf-8;base64," + base64;
        mOriginalUrl = url;
        navigate(url, "");
    }

    @Override
    public void loadDataWithBaseURL(String baseUrl, String data, String mimeType,
            String encoding, String historyUrl) {
        if (data == null) data = "";
        if (mimeType == null || mimeType.isEmpty()) mimeType = "text/html";
        byte[] bytes = "base64".equalsIgnoreCase(encoding)
                ? Base64.decode(data, Base64.DEFAULT) : data.getBytes(StandardCharsets.UTF_8);
        if (baseUrl == null || baseUrl.isEmpty() || baseUrl.startsWith("data:")
                || baseUrl.startsWith("about:")) {
            if (mHost == null) return;
            mOriginalUrl = historyUrl != null ? historyUrl : "about:blank";
            mHost.navigateToString(new String(bytes, StandardCharsets.UTF_8));
            return;
        }
        // The base URL is loaded, and its main document is answered with the data.
        String url = FileUrls.toWebView2(baseUrl);
        mDataUrl = url;
        mData = bytes;
        mDataMime = mimeType;
        mOriginalUrl = baseUrl;
        navigate(url, "");
    }

    @Override
    public void evaluateJavaScript(String script, ValueCallback<String> resultCallback) {
        if (mHost == null) {
            if (resultCallback != null) mUi.post(() -> resultCallback.onReceiveValue(null));
            return;
        }
        long id = mNextCallback.getAndIncrement();
        if (resultCallback != null) mScriptCallbacks.put(id, resultCallback);
        mHost.executeScript(script, id);
    }

    @Override
    public void saveWebArchive(String filename) {
        saveWebArchive(filename, false, null);
    }

    /**
     * An MHTML web archive (the DevTools protocol's Page.captureSnapshot), as Android's
     * WebView writes it; {@code autoname}: basename is a directory and the file is named
     * after the page.
     */
    @Override
    public void saveWebArchive(String basename, boolean autoname, ValueCallback<String> callback) {
        if (mHost == null || basename == null) {
            if (callback != null) mUi.post(() -> callback.onReceiveValue(null));
            return;
        }
        devTools("Page.captureSnapshot", "{\"format\":\"mhtml\"}", json -> {
            String path = null;
            try {
                if (json != null) {
                    String data = new JSONObject(json).getString("data");
                    File file = new File(basename);
                    if (autoname) {
                        String name = Uri.parse(mUrl != null ? mUrl : "").getLastPathSegment();
                        if (name == null || name.isEmpty()) name = "index";
                        name = name.replaceAll("[^A-Za-z0-9._-]", "_");
                        if (!name.endsWith(".mht")) name = name + ".mht";
                        file = new File(basename, name);
                        for (int i = 1; file.exists(); i++) {
                            file = new File(basename, name.replace(".mht", "-" + i + ".mht"));
                        }
                    }
                    try (OutputStream out = new FileOutputStream(file)) {
                        out.write(data.getBytes(StandardCharsets.UTF_8));
                    }
                    path = file.getPath();
                }
            } catch (Exception e) {
                GesherWebView.log("saveWebArchive(" + basename + "): " + e);
                path = null;
            }
            if (callback != null) callback.onReceiveValue(path);
        });
    }

    /** A DevTools protocol method; the callback gets its result (JSON) or null, on the UI thread. */
    private void devTools(String method, String params, ValueCallback<String> callback) {
        if (mHost == null) {
            if (callback != null) mUi.post(() -> callback.onReceiveValue(null));
            return;
        }
        long id = mNextCallback.getAndIncrement();
        if (callback != null) mDevToolsCallbacks.put(id, callback);
        mHost.callDevTools(method, params, id);
    }

    /** Waits (bounded) for a result answered on the WebView thread; null when none came. */
    private Sync await(long id, Runnable start, long millis) {
        if (mHost == null || WebView2Host.onWebViewThread()) return null;
        Sync sync = new Sync();
        mSync.put(id, sync);
        start.run();
        try {
            if (!sync.done.await(millis, TimeUnit.MILLISECONDS)) {
                mSync.remove(id);
                return null;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            mSync.remove(id);
            return null;
        }
        return sync;
    }

    @Override
    public void stopLoading() {
        if (mHost != null) mHost.run(WebView2Host.STOP);
    }

    @Override
    public void reload() {
        if (mHost != null) mHost.run(WebView2Host.RELOAD);
    }

    @Override
    public boolean canGoBack() {
        return mCanGoBack;
    }

    @Override
    public void goBack() {
        if (mHost != null) mHost.run(WebView2Host.GO_BACK);
    }

    @Override
    public boolean canGoForward() {
        return mCanGoForward;
    }

    @Override
    public void goForward() {
        if (mHost != null) mHost.run(WebView2Host.GO_FORWARD);
    }

    @Override
    public boolean canGoBackOrForward(int steps) {
        if (steps == 0 || (steps == -1 && mCanGoBack) || (steps == 1 && mCanGoForward)) {
            return true;
        }
        return mHistory.entryId(steps) >= 0;
    }

    @Override
    public void goBackOrForward(int steps) {
        if (steps == -1) {
            goBack();
        } else if (steps == 1) {
            goForward();
        } else if (steps != 0) {
            // Page.navigateToHistoryEntry: several steps at once.
            int entry = mHistory.entryId(steps);
            if (entry >= 0) devTools("Page.navigateToHistoryEntry", "{\"entryId\":" + entry + "}", null);
        }
    }

    /** The history (Page.getNavigationHistory), read again after each change. */
    private void refreshHistory() {
        devTools("Page.getNavigationHistory", "{}", json -> {
            GesherBackForwardList list = json != null ? GesherBackForwardList.parse(json) : null;
            if (list != null) mHistory = list;
        });
    }

    @Override
    public boolean isPrivateBrowsingEnabled() {
        return false;
    }

    @Override
    public boolean pageUp(boolean top) {
        evaluateJavaScript(top ? "window.scrollTo(0, 0)" : "window.scrollBy(0, -window.innerHeight)",
                null);
        return true;
    }

    @Override
    public boolean pageDown(boolean bottom) {
        evaluateJavaScript(bottom ? "window.scrollTo(0, document.body.scrollHeight)"
                : "window.scrollBy(0, window.innerHeight)", null);
        return true;
    }

    @Override
    public void insertVisualStateCallback(long requestId, WebView.VisualStateCallback callback) {
        if (callback != null) mUi.post(() -> callback.onComplete(requestId));
    }

    @Override
    public void clearView() {
    }

    /**
     * What the WebView shows (CapturePreview, waited for up to 3 s) drawn into a Picture of
     * the view's size; an empty Picture when it does not come.
     */
    @Override
    public Picture capturePicture() {
        Picture picture = new Picture();
        int w = Math.max(1, mView.getWidth());
        int h = Math.max(1, mView.getHeight());
        long id = mNextCallback.getAndIncrement();
        Sync sync = await(id, () -> mHost.capturePreview(id), 3000);
        Bitmap bmp = sync != null && sync.hresult >= 0 && sync.bytes != null
                ? BitmapFactory.decodeByteArray(sync.bytes, 0, sync.bytes.length) : null;
        Canvas canvas = picture.beginRecording(w, h);
        if (bmp != null) {
            canvas.drawBitmap(bmp, null, new Rect(0, 0, w, h), null);
        }
        picture.endRecording();
        return picture;
    }

    @Override
    public PrintDocumentAdapter createPrintDocumentAdapter(String documentName) {
        return null;
    }

    @Override
    public float getScale() {
        return mContext.getResources().getDisplayMetrics().density;
    }

    @Override
    public void setInitialScale(int scaleInPercent) {
        if (mHost != null && scaleInPercent > 0) {
            float density = mContext.getResources().getDisplayMetrics().density;
            mHost.setZoom(scaleInPercent / 100.0 / (density > 0 ? density : 1));
        }
    }

    @Override
    public void invokeZoomPicker() {
    }

    @Override
    public WebView.HitTestResult getHitTestResult() {
        return new WebView.HitTestResult();
    }

    @Override
    public void requestFocusNodeHref(Message hrefMsg) {
    }

    @Override
    public void requestImageRef(Message msg) {
    }

    @Override
    public String getUrl() {
        return mUrl;
    }

    @Override
    public String getOriginalUrl() {
        return mOriginalUrl != null ? mOriginalUrl : mUrl;
    }

    @Override
    public String getTitle() {
        return mTitle;
    }

    @Override
    public Bitmap getFavicon() {
        return null;
    }

    @Override
    public String getTouchIconUrl() {
        return null;
    }

    @Override
    public int getProgress() {
        return mProgress;
    }

    @Override
    public int getContentHeight() {
        return mContentHeight;
    }

    @Override
    public int getContentWidth() {
        return mView.getWidth();
    }

    @Override
    public void pauseTimers() {
    }

    @Override
    public void resumeTimers() {
    }

    @Override
    public void onPause() {
        mPaused = true;
    }

    @Override
    public void onResume() {
        mPaused = false;
    }

    @Override
    public boolean isPaused() {
        return mPaused;
    }

    @Override
    public void freeMemory() {
    }

    @Override
    public void clearCache(boolean includeDiskFiles) {
        WebView2Host.clearBrowsingData(includeDiskFiles
                ? WebView2Host.DATA_DISK_CACHE | WebView2Host.DATA_CACHE_STORAGE
                : WebView2Host.DATA_CACHE_STORAGE, null);
    }

    @Override
    public void clearFormData() {
    }

    @Override
    public void clearHistory() {
        devTools("Page.resetNavigationHistory", "{}", json -> refreshHistory());
    }

    @Override
    public void clearSslPreferences() {
    }

    @Override
    public WebBackForwardList copyBackForwardList() {
        return mHistory;
    }

    @Override
    public void setFindListener(WebView.FindListener listener) {
        mFindListener = listener;
    }

    @Override
    public void findNext(boolean forward) {
        if (mHost == null || mFindTerm == null) return;
        if (mNativeFind) {
            mHost.find(forward ? WebView2Host.FIND_NEXT : WebView2Host.FIND_PREVIOUS, "", false,
                    false);
        } else {
            scriptFind(forward ? "next" : "previous", mFindTerm);
        }
    }

    /** Deprecated and synchronous: the script's count, waited for up to 3 s. */
    @Override
    public int findAll(String find) {
        mFindTerm = find;
        long id = mNextCallback.getAndIncrement();
        Sync sync = await(id, () -> mHost.executeScript(PageScripts.find("start", find), id), 3000);
        if (sync == null || sync.hresult < 0) return 0;
        try {
            JSONObject r = new JSONObject(sync.text);
            int count = r.optInt("count");
            WebView.FindListener listener = mFindListener;
            if (listener != null) {
                mUi.post(() -> listener.onFindResultReceived(r.optInt("active", -1) < 0 ? 0
                        : r.optInt("active"), count, true));
            }
            return count;
        } catch (Exception e) {
            return 0;
        }
    }

    /** WebView2's Find API when the runtime has it, else PageScripts.find. */
    @Override
    public void findAllAsync(String find) {
        mFindTerm = find;
        if (mHost == null) return;
        if (find == null || find.isEmpty()) {
            clearMatches();
            return;
        }
        if (mNativeFind) {
            mHost.find(WebView2Host.FIND_START, find, false, false);
        } else {
            scriptFind("start", find);
        }
    }

    private void scriptFind(String op, String term) {
        evaluateJavaScript(PageScripts.find(op, term), json -> {
            WebView.FindListener listener = mFindListener;
            if (listener == null || json == null || "clear".equals(op)) return;
            try {
                JSONObject r = new JSONObject(json);
                int active = r.optInt("active", -1);
                listener.onFindResultReceived(Math.max(0, active), r.optInt("count"), true);
            } catch (Exception e) {
                // not a result
            }
        });
    }

    @Override
    public boolean showFindDialog(String text, boolean showIme) {
        return false;
    }

    @Override
    public void clearMatches() {
        mFindTerm = null;
        if (mHost == null) return;
        if (mNativeFind) {
            mHost.find(WebView2Host.FIND_STOP, "", false, false);
        } else {
            scriptFind("clear", "");
        }
    }

    @Override
    public void documentHasImages(Message response) {
        if (response != null) {
            response.arg1 = 0;
            response.sendToTarget();
        }
    }

    @Override
    public void setWebViewClient(WebViewClient client) {
        mClient = client != null ? client : new WebViewClient();
        Class<?> c = mClient.getClass();
        mOverridesNavigation = overrides(c, "shouldOverrideUrlLoading", WebView.class,
                WebResourceRequest.class)
                || overrides(c, "shouldOverrideUrlLoading", WebView.class, String.class);
        boolean intercepts = overrides(c, "shouldInterceptRequest", WebView.class,
                WebResourceRequest.class)
                || overrides(c, "shouldInterceptRequest", WebView.class, String.class);
        if (intercepts != mInterceptsRequests) {
            mInterceptsRequests = intercepts;
            applySettings();
        }
    }

    private static boolean overrides(Class<?> c, String name, Class<?>... params) {
        try {
            return c.getMethod(name, params).getDeclaringClass() != WebViewClient.class;
        } catch (NoSuchMethodException e) {
            return false;
        }
    }

    @Override
    public WebViewClient getWebViewClient() {
        return mClient;
    }

    @Override
    public WebViewRenderProcess getWebViewRenderProcess() {
        return null;
    }

    @Override
    public void setWebViewRenderProcessClient(Executor executor,
            WebViewRenderProcessClient client) {
    }

    @Override
    public WebViewRenderProcessClient getWebViewRenderProcessClient() {
        return null;
    }

    @Override
    public void setDownloadListener(DownloadListener listener) {
        mDownloadListener = listener;
    }

    @Override
    public void setWebChromeClient(WebChromeClient client) {
        mChrome = client;
    }

    @Override
    public WebChromeClient getWebChromeClient() {
        return mChrome;
    }

    @Override
    @SuppressWarnings("deprecation")
    public void setPictureListener(WebView.PictureListener listener) {
    }

    @Override
    public void addJavascriptInterface(Object obj, String interfaceName) {
        if (obj == null || interfaceName == null) return;
        mInterfaces.put(interfaceName, obj);
        updateDocumentScript();
    }

    @Override
    public void removeJavascriptInterface(String interfaceName) {
        if (interfaceName == null) return;
        mInterfaces.remove(interfaceName);
        updateDocumentScript();
    }

    @Override
    public WebMessagePort[] createWebMessageChannel() {
        return GesherWebMessagePort.pair();
    }

    /**
     * A message event on the page's window (PageScripts.PORTS), with the message's ports as
     * MessagePorts of the page; targetOrigin "*" (or empty) means any page.
     */
    @Override
    public void postMessageToMainFrame(WebMessage message, Uri targetOrigin) {
        if (message == null || mHost == null) return;
        String origin = targetOrigin == null || Uri.EMPTY.equals(targetOrigin)
                || "*".equals(targetOrigin.toString()) ? "*" : webView2Origin(targetOrigin);
        try {
            JSONObject m = new JSONObject();
            m.put("gesher", "post");
            WebMessagePayload.write(m, message);
            m.put("origin", origin);
            m.put("ports", transfer(message.getPorts()));
            mHost.postWebMessage(m.toString());
        } catch (org.json.JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    /** "scheme://host[:port]" as the page sees it (file: pages are FileUrls' origin). */
    private static String webView2Origin(Uri origin) {
        String s = origin.toString();
        if (s.startsWith("file:")) return FileUrls.toWebView2("file:///").replaceAll("/+$", "");
        return s.replaceAll("/+$", "");
    }

    /** The ports go to this WebView's page: their ids. */
    private JSONArray transfer(WebMessagePort[] ports) {
        JSONArray ids = new JSONArray();
        if (ports == null) return ids;
        for (WebMessagePort p : ports) {
            if (!(p instanceof GesherWebMessagePort)) {
                throw new IllegalArgumentException("not a port of this WebView");
            }
            GesherWebMessagePort port = (GesherWebMessagePort) p;
            port.transferTo(this);
            mPagePorts.put(port.mId, port);
            ids.put(port.mId);
        }
        return ids;
    }

    /** The app posts to the end that is in the page (GesherWebMessagePort). */
    void postToPagePort(GesherWebMessagePort port, WebMessage message) {
        WebView2Host host = mHost;
        if (host == null) return;
        try {
            JSONObject m = new JSONObject();
            m.put("gesher", "port");
            m.put("id", port.mId);
            WebMessagePayload.write(m, message);
            m.put("ports", transfer(message.getPorts()));
            host.postWebMessage(m.toString());
        } catch (org.json.JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    void closePagePort(GesherWebMessagePort port) {
        mPagePorts.remove(port.mId);
        WebView2Host host = mHost;
        if (host != null) {
            host.postWebMessage("{\"gesher\":\"close\",\"id\":" + JSONObject.quote(port.mId) + "}");
        }
    }

    @Override
    public WebSettings getSettings() {
        return mSettings;
    }

    @Override
    public void setMapTrackballToArrowKeys(boolean setMap) {
    }

    @Override
    public void flingScroll(int vx, int vy) {
    }

    @Override
    public View getZoomControls() {
        return null;
    }

    @Override
    public boolean canZoomIn() {
        return false;
    }

    @Override
    public boolean canZoomOut() {
        return false;
    }

    @Override
    public boolean zoomBy(float zoomFactor) {
        return false;
    }

    @Override
    public boolean zoomIn() {
        return false;
    }

    @Override
    public boolean zoomOut() {
        return false;
    }

    @Override
    public void dumpViewHierarchyWithProperties(BufferedWriter out, int level) {
    }

    @Override
    public View findHierarchyView(String className, int hashCode) {
        return null;
    }

    @Override
    public void setRendererPriorityPolicy(int rendererRequestedPriority,
            boolean waivedWhenNotVisible) {
    }

    @Override
    public int getRendererRequestedPriority() {
        return WebView.RENDERER_PRIORITY_IMPORTANT;
    }

    @Override
    public boolean getRendererPriorityWaivedWhenNotVisible() {
        return false;
    }

    @Override
    public ViewDelegate getViewDelegate() {
        return this;
    }

    @Override
    public ScrollDelegate getScrollDelegate() {
        return this;
    }

    @Override
    public void notifyFindDialogDismissed() {
    }

    // ---- Where the WebView is ----

    private void addListeners() {
        ViewTreeObserver o = mView.getViewTreeObserver();
        o.addOnPreDrawListener(mPreDraw);
        o.addOnScrollChangedListener(mScrolled);
        o.addOnGlobalLayoutListener(mLaidOut);
    }

    private void removeListeners() {
        ViewTreeObserver o = mView.getViewTreeObserver();
        o.removeOnPreDrawListener(mPreDraw);
        o.removeOnScrollChangedListener(mScrolled);
        o.removeOnGlobalLayoutListener(mLaidOut);
    }

    /** Moves the WebView2 window to the view's place in its Win32 window (if it moved). */
    private void updateBounds() {
        if (mHost == null || mDestroyed || !mAttached) return;
        long hwnd = GesherWebView.hwndOf(mView);
        mView.getLocationInWindow(mLocation);
        int w = mView.getWidth();
        int h = mView.getHeight();
        boolean visible = hwnd != 0 && mView.isShown()
                && mView.getWindowVisibility() == View.VISIBLE
                && mView.getGlobalVisibleRect(mClip);
        if (!visible) mClip.setEmpty();
        long[] now = {hwnd, mLocation[0], mLocation[1], w, h, mClip.left, mClip.top,
                mClip.width(), mClip.height(), visible ? 1 : 0};
        mHoleCount = 0;
        int[] holes = visible ? findHoles() : new int[0];
        if (mPlacedOnce && Arrays.equals(now, mPlaced) && Arrays.equals(holes, mPlacedHoles)) {
            return;
        }
        System.arraycopy(now, 0, mPlaced, 0, now.length);
        if (!Arrays.equals(holes, mPlacedHoles)) {
            StringBuilder what = new StringBuilder();
            what.append(mHoleCount).append(" view(s) over the WebView cut out of its window");
            for (int i = 0; i + 6 < holes.length; i += 7 + 2 * holes[i + 6]) {
                what.append(i == 0 ? ": " : "; ").append(shapeName(holes[i])).append(' ')
                        .append(holes[i + 1]).append(',').append(holes[i + 2]).append('-')
                        .append(holes[i + 3]).append(',').append(holes[i + 4]);
            }
            String line = what.toString();
            if (!line.equals(mPlacedHolesLog)) GesherWebView.log(line);
            mPlacedHolesLog = line;
        }
        mPlacedHoleCount = mHoleCount;
        mPlacedHoles = holes;
        mPlacedOnce = true;
        mHost.setBounds(hwnd, mLocation[0], mLocation[1], w, h, mClip.left, mClip.top,
                mClip.width(), mClip.height(), visible, holes.length > 0 ? holes : null);
        if (GesherWebView.DEBUG) {
            GesherWebView.log("bounds: hwnd 0x" + Long.toHexString(hwnd) + " view "
                    + mLocation[0] + "," + mLocation[1] + " " + w + "x" + h + ", visible "
                    + (visible ? mClip.toShortString() : "no"));
        }
    }

    /**
     * The views drawn over the WebView (a floating action button, a snackbar): the WebView2
     * window is a child window above the app's surface, so these places are cut out of it
     * and the app's pixels show there. Siblings after the WebView's branch (or with a higher
     * Z) in each ancestor, down to views with a background or without children.
     */
    private int[] findHoles() {
        mHoleCount = 0;
        mShapeLength = 0;
        // The WebView's own outline and its ancestors' when they clip to it (rounded
        // corners): the WebView2 window keeps only that shape.
        for (View v = mView; v != null; ) {
            if (v.getClipToOutline()) addShape(v, true);
            android.view.ViewParent p = v.getParent();
            v = p instanceof View ? (View) p : null;
        }
        View child = mView;
        android.view.ViewParent parent = mView.getParent();
        while (parent instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) parent;
            int index = g.indexOfChild(child);
            float z = child.getZ();
            for (int i = 0; i < g.getChildCount(); i++) {
                View v = g.getChildAt(i);
                if (v == child) continue;
                float vz = v.getZ();
                if (vz > z || (i > index && vz >= z)) addHoles(v, 0);
            }
            child = g;
            parent = g.getParent();
        }
        return Arrays.copyOf(mHoles, mShapeLength);
    }

    private void addHoles(View v, int depth) {
        if (v.getVisibility() != View.VISIBLE || v.getAlpha() <= 0f || mHoleCount >= 32) return;
        if (!v.getGlobalVisibleRect(mHoleRect) || !Rect.intersects(mHoleRect, mClip)) return;
        if (v instanceof ViewGroup && v.getBackground() == null && depth < 16
                && ((ViewGroup) v).getChildCount() > 0) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) addHoles(g.getChildAt(i), depth + 1);
            return;
        }
        addShape(v, false);
        mHoleCount++;
    }

    private int mPlacedHoleCount;
    private String mPlacedHolesLog = "";

    private static String shapeName(int kind) {
        switch (kind) {
            case WebView2Host.HOLE_ROUND_RECT: return "round rect";
            case WebView2Host.HOLE_OVAL: return "oval";
            case WebView2Host.HOLE_POLYGON: return "polygon";
            case WebView2Host.CLIP_ROUND_RECT: return "clipped to a round rect";
            case WebView2Host.CLIP_OVAL: return "clipped to an oval";
            case WebView2Host.CLIP_POLYGON: return "clipped to a polygon";
            default: return "rect";
        }
    }

    private final android.graphics.Outline mOutline = new android.graphics.Outline();
    private final Rect mOutlineRect = new Rect();
    private int mShapeLength;

    /**
     * A view's shape in window coordinates (WebView2Host.setBounds' records): its outline
     * (ViewOutlineProvider: a round button is an oval or a round rect, a path is a polygon),
     * else its visible rectangle. Scale and rotation of the view are not applied.
     */
    private void addShape(View v, boolean clip) {
        int[] at = new int[2];
        v.getLocationInWindow(at);
        mOutline.setEmpty();
        android.view.ViewOutlineProvider provider = v.getOutlineProvider();
        try {
            if (provider != null && v.getWidth() > 0 && v.getHeight() > 0) {
                // The background's bounds are set when the view first draws, which may
                // come after this (pre-draw): as View.drawBackground sets them.
                android.graphics.drawable.Drawable bg = v.getBackground();
                if (bg != null && bg.getBounds().isEmpty()) bg.setBounds(0, 0, v.getWidth(), v.getHeight());
                provider.getOutline(v, mOutline);
            }
        } catch (RuntimeException e) {
            mOutline.setEmpty();
        }
        int kind = clip ? -1 : WebView2Host.HOLE_RECT;
        int radius = 0;
        float[] points = null;
        Rect r = mOutlineRect;
        if (!mOutline.isEmpty() && mOutline.getRect(r)) {
            r.offset(at[0], at[1]);
            float rad = mOutline.getRadius();
            radius = Math.max(0, Math.round(rad));
            if (radius > 0) {
                boolean oval = radius * 2 >= Math.min(r.width(), r.height());
                kind = clip ? (oval ? WebView2Host.CLIP_OVAL : WebView2Host.CLIP_ROUND_RECT)
                        : (oval ? WebView2Host.HOLE_OVAL : WebView2Host.HOLE_ROUND_RECT);
                if (oval && r.width() != r.height()) {
                    // A pill: a round rect with the short side's radius.
                    kind = clip ? WebView2Host.CLIP_ROUND_RECT : WebView2Host.HOLE_ROUND_RECT;
                    radius = Math.min(r.width(), r.height()) / 2;
                } else if (oval) {
                    radius = r.width() / 2;
                }
            }
        } else if (!mOutline.isEmpty() && mOutline.mMode == android.graphics.Outline.MODE_PATH
                && mOutline.mPath != null) {
            try {
                points = mOutline.mPath.approximate(0.5f);
            } catch (RuntimeException | UnsatisfiedLinkError e) {
                points = null;
            }
            android.graphics.RectF bounds = new android.graphics.RectF();
            mOutline.mPath.computeBounds(bounds, true);
            r.set((int) Math.floor(bounds.left) + at[0], (int) Math.floor(bounds.top) + at[1],
                    (int) Math.ceil(bounds.right) + at[0], (int) Math.ceil(bounds.bottom) + at[1]);
            if (points != null && points.length >= 9) {
                kind = clip ? WebView2Host.CLIP_POLYGON : WebView2Host.HOLE_POLYGON;
            } else {
                points = null;
            }
        } else {
            if (clip) return;  // nothing to keep but the rectangle
            r.set(mHoleRect);
        }
        if (kind == -1) return;  // a clip to a plain rectangle: the clip rect does it
        if (kind == WebView2Host.HOLE_RECT) {
            r.intersect(mHoleRect);
            r.intersect(mClip);
        }
        int n = points != null ? points.length / 3 : 0;
        int need = mShapeLength + 7 + 2 * n;
        if (mHoles.length < need) mHoles = Arrays.copyOf(mHoles, Math.max(need, mHoles.length * 2));
        int i = mShapeLength;
        mHoles[i++] = kind;
        mHoles[i++] = r.left;
        mHoles[i++] = r.top;
        mHoles[i++] = r.right;
        mHoles[i++] = r.bottom;
        mHoles[i++] = radius;
        mHoles[i++] = n;
        for (int k = 0; k < n; k++) {
            mHoles[i++] = Math.round(points[k * 3 + 1]) + at[0];
            mHoles[i++] = Math.round(points[k * 3 + 2]) + at[1];
        }
        mShapeLength = i;
    }

    // ---- ViewDelegate ----

    @Override
    public boolean shouldDelayChildPressedState() {
        return false;
    }

    @Override
    public void onProvideVirtualStructure(android.view.ViewStructure structure) {
    }

    @Override
    public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        // The page's nodes from WebView2's UI Automation tree (ADR 0058).
        if (mHost == null) return null;
        if (mAccessibility == null) mAccessibility = new WebViewAccessibility(this);
        return mAccessibility;
    }

    private WebViewAccessibility mAccessibility;

    @Override
    public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
    }

    @Override
    public void onInitializeAccessibilityEvent(AccessibilityEvent event) {
    }

    @Override
    public boolean performAccessibilityAction(int action, Bundle arguments) {
        return mAccess.super_performAccessibilityAction(action, arguments);
    }

    @Override
    public void setOverScrollMode(int mode) {
    }

    @Override
    public void setScrollBarStyle(int style) {
    }

    @Override
    public void onDrawVerticalScrollBar(Canvas canvas, Drawable scrollBar, int l, int t, int r,
            int b) {
    }

    @Override
    public void onOverScrolled(int scrollX, int scrollY, boolean clampedX, boolean clampedY) {
    }

    @Override
    public void onWindowVisibilityChanged(int visibility) {
        updateBounds();
    }

    @Override
    public void onDraw(Canvas canvas) {
        // WebView2 draws in its own window over this view.
    }

    @Override
    public void setLayoutParams(ViewGroup.LayoutParams layoutParams) {
        mAccess.super_setLayoutParams(layoutParams);
    }

    @Override
    public boolean performLongClick() {
        return mAccess.super_performLongClick();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        return null;
    }

    @Override
    public boolean onDragEvent(DragEvent event) {
        return false;
    }

    @Override
    public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) {
        return false;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        return false;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        return false;
    }

    @Override
    public void onAttachedToWindow() {
        mAttached = true;
        mPlacedOnce = false;
        addListeners();
        updateBounds();
    }

    @Override
    public void onDetachedFromWindow() {
        mAttached = false;
        removeListeners();
        mPlacedOnce = false;
        if (mHost != null && !mDestroyed) {
            // Out of the window before it goes (bounded: never wait on the WebView thread
            // for long).
            CountDownLatch parked = new CountDownLatch(1);
            mParked = parked;
            mHost.park(1);
            try {
                parked.await(1, TimeUnit.SECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    @Override
    public void onVisibilityChanged(View changedView, int visibility) {
        updateBounds();
    }

    @Override
    public void onWindowFocusChanged(boolean hasWindowFocus) {
    }

    @Override
    public void onFocusChanged(boolean focused, int direction, Rect previouslyFocusedRect) {
        if (mHost != null) mHost.run(focused ? WebView2Host.FOCUS : WebView2Host.BLUR);
    }

    @Override
    public boolean setFrame(int left, int top, int right, int bottom) {
        return mAccess.super_setFrame(left, top, right, bottom);
    }

    @Override
    public void onSizeChanged(int w, int h, int ow, int oh) {
        updateBounds();
    }

    @Override
    public void onScrollChanged(int l, int t, int oldl, int oldt) {
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        return mAccess.super_dispatchKeyEvent(event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        // Input over the WebView reaches WebView2's window directly.
        return false;
    }

    @Override
    public boolean onHoverEvent(MotionEvent event) {
        return mAccess.super_onHoverEvent(event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        return mAccess.super_onGenericMotionEvent(event);
    }

    @Override
    public boolean onTrackballEvent(MotionEvent ev) {
        return false;
    }

    @Override
    public boolean requestFocus(int direction, Rect previouslyFocusedRect) {
        return mAccess.super_requestFocus(direction, previouslyFocusedRect);
    }

    @Override
    public void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int w = View.getDefaultSize(0, widthMeasureSpec);
        int h = View.MeasureSpec.getMode(heightMeasureSpec) == View.MeasureSpec.UNSPECIFIED
                ? Math.round(mContentHeight * mContext.getResources().getDisplayMetrics().density)
                : View.getDefaultSize(0, heightMeasureSpec);
        mAccess.setMeasuredDimension(w, h);
    }

    @Override
    public boolean requestChildRectangleOnScreen(View child, Rect rect, boolean immediate) {
        return false;
    }

    @Override
    public void setBackgroundColor(int color) {
        if (mHost != null) mHost.setBackground(color);
    }

    @Override
    public void setLayerType(int layerType, Paint paint) {
    }

    @Override
    public void preDispatchDraw(Canvas canvas) {
    }

    @Override
    public void onStartTemporaryDetach() {
    }

    @Override
    public void onFinishTemporaryDetach() {
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
    }

    @Override
    public Handler getHandler(Handler originalHandler) {
        return originalHandler;
    }

    @Override
    public View findFocus(View originalFocusedView) {
        return originalFocusedView;
    }

    // ---- ScrollDelegate ----

    @Override
    public int computeHorizontalScrollRange() {
        return mView.getWidth();
    }

    @Override
    public int computeHorizontalScrollOffset() {
        return 0;
    }

    @Override
    public int computeVerticalScrollRange() {
        return mView.getHeight();
    }

    @Override
    public int computeVerticalScrollOffset() {
        return 0;
    }

    @Override
    public int computeVerticalScrollExtent() {
        return mView.getHeight();
    }

    @Override
    public void computeScroll() {
        mAccess.super_computeScroll();
    }

    // ---- WebView2Host.Client (the WebView thread) ----

    @Override
    public void onEvent(int type, long id, int arg, String a, String b, String c, byte[] bytes) {
        if (type == WebView2Host.EVENT_PARKED) {
            CountDownLatch parked = mParked;
            if (parked != null) parked.countDown();
            return;
        }
        if (type == WebView2Host.EVENT_SCRIPT_RESULT || type == WebView2Host.EVENT_CAPTURE
                || type == WebView2Host.EVENT_DEVTOOLS_RESULT) {
            // A thread waits for it (capturePicture, findAll): not through the UI thread,
            // which may be the one waiting.
            Sync sync = mSync.remove(id);
            if (sync != null) {
                sync.hresult = arg;
                sync.text = a;
                sync.bytes = bytes;
                sync.done.countDown();
                return;
            }
        }
        mUi.post(() -> handleEvent(type, id, arg, a, b, c, bytes));
    }

    @Override
    public boolean onNavigationStarting(String url, boolean userInitiated, boolean redirect) {
        if (!mOverridesNavigation) return false;
        String appUrl = FileUrls.fromWebView2(url);
        final WebViewClient client = mClient;
        FutureTask<Boolean> ask = new FutureTask<>(() -> client.shouldOverrideUrlLoading(mView,
                new Request(appUrl, true, redirect, userInitiated, "GET", null)));
        mUi.post(ask);
        try {
            boolean cancel = ask.get(WAIT_FOR_UI_MS, TimeUnit.MILLISECONDS);
            if (cancel) GesherWebView.log("shouldOverrideUrlLoading took " + appUrl);
            return cancel;
        } catch (Exception e) {
            GesherWebView.log("shouldOverrideUrlLoading(" + appUrl + "): no answer (" + e
                    + "); loading it");
            return false;
        }
    }

    @Override
    public void onResourceRequest(long id, String url, String method, String headers,
            int context, boolean mainFrame) {
        sRequests.execute(() -> answerRequest(id, url, method, headers, context, mainFrame));
    }

    @Override
    public String onBridgeCall(String object, String method, String argsJson) {
        return JsBridge.call(mInterfaces, mRequireAnnotation, object, method, argsJson);
    }

    // ---- Requests (a worker thread) ----

    private static final int CONTEXT_DOCUMENT = 1;
    private static final int CONTEXT_IMAGE = 3;

    private void answerRequest(long id, String url, String method, String headers, int context,
            boolean mainFrame) {
        WebView2Host host = mHost;
        if (host == null) return;
        WebResourceResponse response = null;
        try {
            String appUrl = FileUrls.fromWebView2(url);
            byte[] data = mData;
            if (data != null && context == CONTEXT_DOCUMENT && url.equals(mDataUrl)) {
                mData = null;
                mDataUrl = null;
                response = new WebResourceResponse(mDataMime, "UTF-8",
                        new java.io.ByteArrayInputStream(data));
            }
            if (response == null && mInterceptsRequests) {
                response = mClient.shouldInterceptRequest(mView,
                        new Request(appUrl, mainFrame, false, false, method, parseHeaders(headers)));
            }
            if (response == null && FileUrls.isFile(url)) {
                response = FileUrls.load(mContext, appUrl,
                        mSettings.getAllowFileAccess() || appUrl.startsWith("file:///android_"));
            }
            if (response == null && !FileUrls.isFile(url) && blocked(url, context)) {
                response = new WebResourceResponse("text/plain", null, 403, "Blocked",
                        null, new java.io.ByteArrayInputStream(new byte[0]));
            }
        } catch (RuntimeException e) {
            Log.e(TAG, "answering " + url, e);
            response = null;
        }
        if (response == null) {
            host.respond(id, 0, "", "", null);
            return;
        }
        int status = response.getStatusCode() > 0 ? response.getStatusCode() : 200;
        String reason = response.getReasonPhrase() != null ? response.getReasonPhrase() : "OK";
        StringBuilder h = new StringBuilder();
        String mime = response.getMimeType();
        if (mime != null) {
            h.append("Content-Type: ").append(mime);
            if (response.getEncoding() != null) h.append("; charset=").append(response.getEncoding());
            h.append("\r\n");
        }
        Map<String, String> extra = response.getResponseHeaders();
        if (extra != null) {
            for (Map.Entry<String, String> e : extra.entrySet()) {
                if (mime != null && "content-type".equalsIgnoreCase(e.getKey())) continue;
                h.append(e.getKey()).append(": ").append(e.getValue()).append("\r\n");
            }
        }
        host.respond(id, status, reason, h.toString(), readAll(response.getData()));
    }

    private boolean blocked(String url, int context) {
        if (!url.startsWith("http:") && !url.startsWith("https:")) return false;
        if (mSettings.getBlockNetworkLoads()) return true;
        return context == CONTEXT_IMAGE
                && (mSettings.getBlockNetworkImage() || !mSettings.getLoadsImagesAutomatically());
    }

    private static byte[] readAll(InputStream in) {
        if (in == null) return new byte[0];
        try (InputStream s = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            int n;
            while ((n = s.read(buf)) > 0) out.write(buf, 0, n);
            return out.toByteArray();
        } catch (Exception e) {
            return new byte[0];
        }
    }

    private static Map<String, String> parseHeaders(String headers) {
        Map<String, String> out = new LinkedHashMap<>();
        if (headers == null) return out;
        for (String line : headers.split("\r\n")) {
            int colon = line.indexOf(':');
            if (colon > 0) out.put(line.substring(0, colon).trim(), line.substring(colon + 1).trim());
        }
        return out;
    }

    // ---- Events (the WebView's Looper) ----

    private void setProgress(int progress) {
        if (progress == mProgress) return;
        mProgress = progress;
        WebChromeClient chrome = mChrome;
        if (chrome != null) chrome.onProgressChanged(mView, progress);
    }

    private void handleEvent(int type, long id, int arg, String a, String b, String c,
            byte[] bytes) {
        if (mDestroyed && type != WebView2Host.EVENT_SCRIPT_RESULT) return;
        WebChromeClient chrome = mChrome;
        switch (type) {
            case WebView2Host.EVENT_READY:
                if (GesherWebView.DEBUG) GesherWebView.log("WebView2 " + a + " ready");
                break;
            case WebView2Host.EVENT_FAILED:
                GesherWebView.log("WebView2: " + a);
                break;
            case WebView2Host.EVENT_PAGE_STARTED: {
                String url = FileUrls.fromWebView2(a);
                mUrl = url;
                // The ports the page had are gone with it.
                for (GesherWebMessagePort p : mPagePorts.values()) p.pageGone();
                mPagePorts.clear();
                mGeolocation.pageGone();
                for (MessageListener l : mMessageListeners.values()) l.replyProxies.clear();
                setProgress(10);
                mClient.onPageStarted(mView, url, null);
                break;
            }
            case WebView2Host.EVENT_PAGE_FINISHED: {
                String url = FileUrls.fromWebView2(a);
                if (url != null && !url.isEmpty()) mUrl = url;
                int http = (int) id;
                Request request = new Request(mUrl, true, false, false, "GET", null);
                if (http >= 400) {
                    mClient.onReceivedHttpError(mView, request, new WebResourceResponse(
                            "text/html", null, http, "HTTP " + http, null, null));
                } else if (arg >= 0 && arg != 14 /* OPERATION_CANCELED */) {
                    final int code = errorCode(arg);
                    final String description = "net::" + errorName(arg);
                    mClient.onReceivedError(mView, request, new WebResourceError() {
                        @Override
                        public int getErrorCode() {
                            return code;
                        }

                        @Override
                        public CharSequence getDescription() {
                            return description;
                        }
                    });
                }
                setProgress(100);
                if (!sFirstPage) {
                    sFirstPage = true;
                    GesherWebView.log("first page loaded: " + mUrl);
                }
                mClient.onPageFinished(mView, mUrl);
                measureContent();
                refreshHistory();
                if (GesherWebView.DEBUG) capturePreviewForLog(mUrl);
                break;
            }
            case WebView2Host.EVENT_SOURCE_CHANGED: {
                String url = FileUrls.fromWebView2(a);
                if (url != null && !url.isEmpty() && !"about:blank".equals(url)) {
                    mUrl = url;
                    mClient.doUpdateVisitedHistory(mView, url, false);
                }
                break;
            }
            case WebView2Host.EVENT_HISTORY:
                mCanGoBack = (arg & 1) != 0;
                mCanGoForward = (arg & 2) != 0;
                refreshHistory();
                break;
            case WebView2Host.EVENT_TITLE:
                mTitle = a;
                if (chrome != null) chrome.onReceivedTitle(mView, a);
                break;
            case WebView2Host.EVENT_CONTENT_LOADING:
                setProgress(Math.max(mProgress, 30));
                break;
            case WebView2Host.EVENT_DOM_CONTENT_LOADED:
                setProgress(Math.max(mProgress, 70));
                break;
            case WebView2Host.EVENT_SCRIPT_RESULT: {
                ValueCallback<String> cb = mScriptCallbacks.remove(id);
                if (cb != null) cb.onReceiveValue(arg >= 0 ? a : null);
                break;
            }
            case WebView2Host.EVENT_DIALOG:
                showDialog(id, arg, a, b, c);
                break;
            case WebView2Host.EVENT_CONSOLE:
                console(a, b);
                break;
            case WebView2Host.EVENT_DOWNLOAD: {
                DownloadListener listener = mDownloadListener;
                if (listener != null) {
                    listener.onDownloadStart(a, mSettings.getUserAgentString(), c, b, id);
                }
                break;
            }
            case WebView2Host.EVENT_FOCUS:
                if (arg == 1) {
                    if (!mView.hasFocus()) mView.requestFocus();
                } else if (arg == 2 || arg == 3) {
                    View next = mView.focusSearch(arg == 2 ? View.FOCUS_FORWARD : View.FOCUS_BACKWARD);
                    if (next != null) next.requestFocus();
                }
                break;
            case WebView2Host.EVENT_PROCESS_FAILED: {
                boolean handled = mClient.onRenderProcessGone(mView, new RenderProcessGoneDetail() {
                    @Override
                    public boolean didCrash() {
                        return true;
                    }

                    @Override
                    public int rendererPriorityAtExit() {
                        return WebView.RENDERER_PRIORITY_IMPORTANT;
                    }
                });
                GesherWebView.log("WebView2's process failed (kind " + arg + ")"
                        + (handled ? "" : "; the app did not handle it"));
                break;
            }
            case WebView2Host.EVENT_NEW_WINDOW:
                newWindow(id, arg != 0, a, "dialog".equals(b));
                break;
            case WebView2Host.EVENT_CLOSE_WINDOW:
                if (chrome != null) chrome.onCloseWindow(mView);
                break;
            case WebView2Host.EVENT_WEB_MESSAGE:
                webMessage(a, b);
                break;
            case WebView2Host.EVENT_DEVTOOLS_RESULT: {
                ValueCallback<String> cb = mDevToolsCallbacks.remove(id);
                if (cb != null) cb.onReceiveValue(arg >= 0 ? a : null);
                break;
            }
            case WebView2Host.EVENT_DEVTOOLS_EVENT:
                if ("Page.fileChooserOpened".equals(a)) fileChooserOpened(b);
                break;
            case WebView2Host.EVENT_PERMISSION:
                permission(id, arg, a);
                break;
            case WebView2Host.EVENT_BASIC_AUTH:
                basicAuth(id, a, b);
                break;
            case WebView2Host.EVENT_CLIENT_CERT:
                clientCert(id, arg, a, b, c);
                break;
            case WebView2Host.EVENT_SERVER_CERT_ERROR:
                serverCertError(id, arg, a, b);
                break;
            case WebView2Host.EVENT_FIND: {
                if ("unsupported".equals(a)) {
                    // An older runtime: the script finds.
                    mNativeFind = false;
                    if (mFindTerm != null) scriptFind("start", mFindTerm);
                    break;
                }
                WebView.FindListener listener = mFindListener;
                // WebView2's active match index is 1-based; Android's ordinal 0-based.
                if (listener != null && mFindTerm != null) {
                    listener.onFindResultReceived(Math.max(0, (int) id - 1), arg,
                            !"count".equals(a) || id > 0);
                }
                break;
            }
            case WebView2Host.EVENT_FILE_DIALOG: {
                ValueCallback<String[]> cb = mFileDialogs.remove(id);
                if (cb != null) cb.onReceiveValue(arg < 0 || a == null || a.isEmpty() ? null : a.split("\n"));
                break;
            }
            case WebView2Host.EVENT_CAPTURE: {
                ValueCallback<String> cb = mScriptCallbacks.remove(id);
                if (cb != null) cb.onReceiveValue(bytes != null ? Base64.encodeToString(bytes,
                        Base64.NO_WRAP) : null);
                break;
            }
            default:
                break;
        }
    }

    // ---- Windows, messages, files, permissions, authentication (the WebView's Looper) ----

    /**
     * window.open: WebChromeClient.onCreateWindow when the settings support multiple
     * windows (the page gets the WebView the app puts in the WebViewTransport, which must
     * not have loaded anything yet); else the page opens in this WebView, as Android's.
     */
    private void newWindow(long id, boolean user, String url, boolean dialog) {
        final WebView2Host host = mHost;
        if (host == null) return;
        WebChromeClient chrome = mChrome;
        String appUrl = url != null ? FileUrls.fromWebView2(url) : "";
        if (!mSettings.supportMultipleWindows() || chrome == null) {
            host.completeNewWindow(id, null);
            if (!appUrl.isEmpty() && !"about:blank".equals(appUrl)) loadUrl(appUrl);
            return;
        }
        if (!user && !mSettings.getJavaScriptCanOpenWindowsAutomatically()) {
            host.completeNewWindow(id, null);
            return;
        }
        final WebView.WebViewTransport transport = mView.new WebViewTransport();
        final AtomicBoolean done = new AtomicBoolean();
        Handler handler = new Handler(mUi.getLooper()) {
            @Override
            public void handleMessage(Message m) {
                if (!done.compareAndSet(false, true)) return;
                WebView w = transport.getWebView();
                WebView2Host window = null;
                if (w != null && w != mView
                        && w.getWebViewProvider() instanceof GesherWebViewProvider) {
                    window = ((GesherWebViewProvider) w.getWebViewProvider()).mHost;
                }
                if (w != null && window == null) {
                    GesherWebView.log("onCreateWindow: the WebView has no WebView2 of its own");
                }
                host.completeNewWindow(id, window);
            }
        };
        Message msg = handler.obtainMessage();
        msg.obj = transport;
        boolean created = false;
        try {
            created = chrome.onCreateWindow(mView, dialog, user, msg);
        } catch (RuntimeException e) {
            Log.e(TAG, "onCreateWindow", e);
        }
        if (!created && done.compareAndSet(false, true)) host.completeNewWindow(id, null);
    }

    /** chrome.webview.postMessage from the page: a page port's message (PageScripts.PORTS). */
    private void webMessage(String json, String source) {
        try {
            Object v = new org.json.JSONTokener(json).nextValue();
            if (!(v instanceof JSONObject)) return;
            JSONObject m = (JSONObject) v;
            if (m.has("gesherGeo")) {
                mGeolocation.onMessage(m, source);
                return;
            }
            String listener = m.optString("gesherListener", null);
            if (listener != null) {
                MessageListener l = mMessageListeners.get(listener);
                if (l != null) SupportLib.deliver(this, l, WebMessagePayload.read(m), source);
                return;
            }
            String id = m.optString("gesherPort", null);
            GesherWebMessagePort port = id != null ? mPagePorts.get(id) : null;
            if (port != null) port.fromPage(WebMessagePayload.read(m));
        } catch (Exception e) {
            // not a port's message
        }
    }

    /**
     * {@code <input type=file>} opened (the DevTools protocol's file chooser interception):
     * WebChromeClient.onShowFileChooser, or, when the app does not handle it, a Windows file
     * dialog. The files reach the input with DOM.setFileInputFiles.
     */
    private void fileChooserOpened(String json) {
        final int node;
        final boolean multiple;
        try {
            JSONObject p = new JSONObject(json);
            node = p.getInt("backendNodeId");
            multiple = "selectMultiple".equals(p.optString("mode"));
        } catch (Exception e) {
            return;
        }
        devTools("DOM.resolveNode", "{\"backendNodeId\":" + node + "}", resolved -> {
            String objectId = null;
            try {
                objectId = new JSONObject(resolved).getJSONObject("object").getString("objectId");
            } catch (Exception e) {
                objectId = null;
            }
            if (objectId == null) {
                showFileChooser(node, null, multiple, "", false);
                return;
            }
            final String oid = objectId;
            devTools("Runtime.callFunctionOn", "{\"objectId\":" + JSONObject.quote(oid)
                    + ",\"returnByValue\":true,\"functionDeclaration\":" + JSONObject.quote(
                            "function() { return {accept: this.accept || '',"
                                    + " capture: this.hasAttribute('capture')}; }") + "}", r -> {
                String accept = "";
                boolean capture = false;
                try {
                    JSONObject v = new JSONObject(r).getJSONObject("result").getJSONObject("value");
                    accept = v.optString("accept");
                    capture = v.optBoolean("capture");
                } catch (Exception e) {
                    // the defaults
                }
                showFileChooser(node, oid, multiple, accept, capture);
            });
        });
    }

    private void showFileChooser(int node, String objectId, boolean multiple, String accept,
            boolean capture) {
        final String[] types = accept == null || accept.trim().isEmpty() ? new String[0]
                : accept.trim().split("\\s*,\\s*");
        final AtomicBoolean answered = new AtomicBoolean();
        ValueCallback<Uri[]> callback = uris -> {
            if (!answered.compareAndSet(false, true)) {
                throw new IllegalStateException("Duplicate showFileChooser result");
            }
            mUi.post(() -> chooseFiles(node, objectId, uris != null ? paths(uris) : null));
        };
        WebChromeClient.FileChooserParams params = new WebChromeClient.FileChooserParams() {
            @Override
            public int getMode() {
                return multiple ? MODE_OPEN_MULTIPLE : MODE_OPEN;
            }

            @Override
            public String[] getAcceptTypes() {
                return types.length > 0 ? types.clone() : new String[] {""};
            }

            @Override
            public boolean isCaptureEnabled() {
                return capture;
            }

            @Override
            public CharSequence getTitle() {
                return null;
            }

            @Override
            public String getFilenameHint() {
                return null;
            }

            @Override
            public Intent createIntent() {
                Intent i = new Intent(Intent.ACTION_GET_CONTENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                String mime = types.length == 1 && types[0].contains("/") ? types[0] : "*/*";
                i.setType(mime);
                if (multiple) i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
                return i;
            }
        };
        WebChromeClient chrome = mChrome;
        boolean handled = false;
        if (chrome != null) {
            try {
                handled = chrome.onShowFileChooser(mView, callback, params);
            } catch (RuntimeException e) {
                Log.e(TAG, "onShowFileChooser", e);
            }
        }
        if (handled || mHost == null) return;
        answered.set(true);
        // Not handled by the app: Gesher's default, a Windows file dialog.
        long id = mNextCallback.getAndIncrement();
        mFileDialogs.put(id, paths -> chooseFiles(node, objectId, paths));
        mHost.showFileDialog(id, multiple, null, fileFilters(types));
        GesherWebView.log("file chooser: a Windows file dialog");
    }

    /** The chosen files (Windows paths) go to the input; none: the chooser is cancelled. */
    private void chooseFiles(int node, String objectId, String[] paths) {
        if (paths == null || paths.length == 0) {
            if (objectId != null) {
                devTools("Runtime.callFunctionOn", "{\"objectId\":" + JSONObject.quote(objectId)
                        + ",\"functionDeclaration\":" + JSONObject.quote("function() {"
                        + " this.dispatchEvent(new Event('cancel', {bubbles: true})); }") + "}",
                        null);
            }
            return;
        }
        JSONArray files = new JSONArray();
        for (String p : paths) files.put(p);
        devTools("DOM.setFileInputFiles", "{\"files\":" + files + ",\"backendNodeId\":" + node
                + "}", r -> {
            if (r == null) GesherWebView.log("file chooser: DOM.setFileInputFiles failed");
        });
    }

    /** The app's Uris as Windows paths: file: as they are, content: copied to the cache. */
    private String[] paths(Uri[] uris) {
        List<String> out = new ArrayList<>();
        for (Uri u : uris) {
            if (u == null) continue;
            try {
                if ("file".equals(u.getScheme()) && u.getPath() != null) {
                    out.add(WebView2Host.windowsPath(u.getPath()));
                } else if ("content".equals(u.getScheme())) {
                    String name = u.getLastPathSegment();
                    if (name == null || name.isEmpty()) name = "upload";
                    name = name.replaceAll("[\\\\/:*?\"<>|]", "_");
                    File dir = new File(mContext.getCacheDir(), "webview-uploads");
                    dir.mkdirs();
                    File f = new File(dir, name);
                    try (InputStream in = mContext.getContentResolver().openInputStream(u);
                         OutputStream o = new FileOutputStream(f)) {
                        byte[] buf = new byte[16384];
                        int n;
                        while (in != null && (n = in.read(buf)) > 0) o.write(buf, 0, n);
                    }
                    out.add(WebView2Host.windowsPath(f.getAbsolutePath()));
                } else {
                    GesherWebView.log("file chooser: cannot upload " + u);
                }
            } catch (Exception e) {
                GesherWebView.log("file chooser: " + u + ": " + e);
            }
        }
        return out.toArray(new String[0]);
    }

    /** IFileDialog filters ("Name|*.a;*.b" lines) for an input's accept types. */
    private static String fileFilters(String[] types) {
        StringBuilder patterns = new StringBuilder();
        for (String t : types) {
            String type = t.trim().toLowerCase(java.util.Locale.ROOT);
            String p;
            if (type.startsWith(".")) {
                p = "*" + type;
            } else if (type.equals("image/*")) {
                p = "*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.svg;*.ico";
            } else if (type.equals("video/*")) {
                p = "*.mp4;*.webm;*.mkv;*.avi;*.mov;*.wmv";
            } else if (type.equals("audio/*")) {
                p = "*.mp3;*.wav;*.ogg;*.oga;*.m4a;*.flac;*.aac;*.wma";
            } else {
                String ext = android.webkit.MimeTypeMap.getSingleton().getExtensionFromMimeType(type);
                p = ext != null ? "*." + ext : null;
            }
            if (p == null) continue;
            if (patterns.length() > 0) patterns.append(';');
            patterns.append(p);
        }
        String all = "All files|*.*";
        return patterns.length() > 0 ? "Accepted files|" + patterns + "\n" + all : all;
    }

    /** COREWEBVIEW2_PERMISSION_KIND. */
    private static final int PERMISSION_MICROPHONE = 1;
    private static final int PERMISSION_CAMERA = 2;
    private static final int PERMISSION_GEOLOCATION = 3;
    private static final int PERMISSION_AUTOPLAY = 9;
    private static final int PERMISSION_MIDI_SYSEX = 11;

    /** The origin ("scheme://host[:port]/") the app sees for a page URL. */
    private static String appOrigin(String webView2Url) {
        Uri u = Uri.parse(FileUrls.fromWebView2(webView2Url != null ? webView2Url : ""));
        if ("file".equals(u.getScheme())) return "file:///";
        String origin = u.getScheme() + "://" + u.getHost() + (u.getPort() > 0 ? ":" + u.getPort() : "");
        return origin + "/";
    }

    /**
     * A page asks for a permission: geolocation through
     * WebChromeClient.onGeolocationPermissionsShowPrompt, camera, microphone and MIDI SysEx
     * through onPermissionRequest, autoplay by WebSettings; the rest is denied, as Android's
     * WebView has none of them.
     */
    private void permission(long id, int kind, String url) {
        final WebView2Host host = mHost;
        if (host == null) return;
        WebChromeClient chrome = mChrome;
        String origin = appOrigin(url);
        switch (kind) {
            case PERMISSION_GEOLOCATION: {
                GesherGeolocationPermissions store = (GesherGeolocationPermissions)
                        GeolocationPermissions.getInstance();
                Boolean retained = store.retained(origin);
                if (!mSettings.geolocationEnabled() || chrome == null || retained != null) {
                    host.completePermission(id, Boolean.TRUE.equals(retained)
                            && mSettings.geolocationEnabled() ? WebView2Host.PERMISSION_ALLOW
                            : WebView2Host.PERMISSION_DENY, false);
                    break;
                }
                AtomicBoolean done = new AtomicBoolean();
                chrome.onGeolocationPermissionsShowPrompt(origin, (o, allow, retain) -> {
                    if (!done.compareAndSet(false, true)) return;
                    if (retain) store.retain(origin, allow);
                    host.completePermission(id, allow ? WebView2Host.PERMISSION_ALLOW
                            : WebView2Host.PERMISSION_DENY, retain);
                });
                break;
            }
            case PERMISSION_CAMERA:
            case PERMISSION_MICROPHONE:
            case PERMISSION_MIDI_SYSEX: {
                String resource = kind == PERMISSION_CAMERA ? PermissionRequest.RESOURCE_VIDEO_CAPTURE
                        : kind == PERMISSION_MICROPHONE ? PermissionRequest.RESOURCE_AUDIO_CAPTURE
                        : PermissionRequest.RESOURCE_MIDI_SYSEX;
                Uri originUri = Uri.parse(origin);
                AtomicBoolean done = new AtomicBoolean();
                PermissionRequest request = new PermissionRequest() {
                    @Override
                    public Uri getOrigin() {
                        return originUri;
                    }

                    @Override
                    public String[] getResources() {
                        return new String[] {resource};
                    }

                    @Override
                    public void grant(String[] resources) {
                        if (!done.compareAndSet(false, true)) return;
                        boolean ok = resources != null && Arrays.asList(resources).contains(resource);
                        host.completePermission(id, ok ? WebView2Host.PERMISSION_ALLOW
                                : WebView2Host.PERMISSION_DENY, false);
                    }

                    @Override
                    public void deny() {
                        if (!done.compareAndSet(false, true)) return;
                        host.completePermission(id, WebView2Host.PERMISSION_DENY, false);
                    }
                };
                if (chrome != null) {
                    chrome.onPermissionRequest(request);  // its default denies
                } else {
                    request.deny();
                }
                break;
            }
            case PERMISSION_AUTOPLAY:
                host.completePermission(id, mSettings.getMediaPlaybackRequiresUserGesture()
                        ? WebView2Host.PERMISSION_DENY : WebView2Host.PERMISSION_ALLOW, false);
                break;
            default:
                host.completePermission(id, WebView2Host.PERMISSION_DENY, false);
                break;
        }
    }

    /** HTTP Basic authentication: WebViewClient.onReceivedHttpAuthRequest (its default cancels). */
    private void basicAuth(long id, String url, String challenge) {
        final WebView2Host host = mHost;
        if (host == null) return;
        String hostName = Uri.parse(url != null ? url : "").getHost();
        String realm = "";
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("realm=\"([^\"]*)\"",
                java.util.regex.Pattern.CASE_INSENSITIVE).matcher(challenge != null ? challenge : "");
        if (m.find()) realm = m.group(1);
        final String[] saved = getHttpAuthUsernamePassword(hostName, realm);
        final AtomicBoolean done = new AtomicBoolean();
        HttpAuthHandler handler = new HttpAuthHandler() {
            @Override
            public boolean useHttpAuthUsernamePassword() {
                return saved != null;
            }

            @Override
            public void cancel() {
                if (done.compareAndSet(false, true)) host.completeBasicAuth(id, false, "", "");
            }

            @Override
            public void proceed(String username, String password) {
                if (done.compareAndSet(false, true)) {
                    host.completeBasicAuth(id, true, username != null ? username : "",
                            password != null ? password : "");
                }
            }
        };
        mClient.onReceivedHttpAuthRequest(mView, handler, hostName, realm);
    }

    /**
     * A server asks for a client certificate: WebViewClient.onReceivedClientCertRequest (its
     * default cancels). WebView2 takes certificates from Windows' store only, so proceed()
     * selects the store's certificate equal to the chain's first; ignore() goes on without one.
     */
    private void clientCert(long id, int port, String hostName, String certsJson, String casJson) {
        final WebView2Host host = mHost;
        if (host == null) return;
        final List<byte[]> ders = new ArrayList<>();
        try {
            JSONArray certs = new JSONArray(certsJson);
            for (int i = 0; i < certs.length(); i++) ders.add(pemToDer(certs.getJSONObject(i).optString("pem")));
        } catch (Exception e) {
            // none
        }
        final List<Principal> principals = new ArrayList<>();
        try {
            JSONArray cas = new JSONArray(casJson);
            for (int i = 0; i < cas.length(); i++) {
                try {
                    principals.add(new javax.security.auth.x500.X500Principal(cas.getString(i)));
                } catch (IllegalArgumentException e) {
                    // not a distinguished name
                }
            }
        } catch (Exception e) {
            // none
        }
        final AtomicBoolean done = new AtomicBoolean();
        ClientCertRequest request = new ClientCertRequest() {
            @Override
            public String[] getKeyTypes() {
                return new String[] {"RSA", "ECDSA"};
            }

            @Override
            public Principal[] getPrincipals() {
                return principals.isEmpty() ? null : principals.toArray(new Principal[0]);
            }

            @Override
            public String getHost() {
                return hostName;
            }

            @Override
            public int getPort() {
                return port;
            }

            @Override
            public void proceed(PrivateKey privateKey, X509Certificate[] chain) {
                if (!done.compareAndSet(false, true)) return;
                int choice = -1;
                try {
                    byte[] want = chain != null && chain.length > 0 ? chain[0].getEncoded() : null;
                    for (int i = 0; want != null && i < ders.size(); i++) {
                        if (Arrays.equals(want, ders.get(i))) choice = i;
                    }
                } catch (Exception e) {
                    choice = -1;
                }
                if (choice < 0) {
                    GesherWebView.log("client certificate: not in Windows' certificate store;"
                            + " going on without one");
                }
                host.completeClientCert(id, choice);
            }

            @Override
            public void ignore() {
                if (done.compareAndSet(false, true)) host.completeClientCert(id, -1);
            }

            @Override
            public void cancel() {
                if (done.compareAndSet(false, true)) host.completeClientCert(id, -2);
            }
        };
        mClient.onReceivedClientCertRequest(mView, request);
    }

    private static byte[] pemToDer(String pem) {
        String body = pem.replaceAll("-----[^-]+-----", "").replaceAll("\\s", "");
        return Base64.decode(body, Base64.DEFAULT);
    }

    /** A certificate error: WebViewClient.onReceivedSslError (its default cancels). */
    private void serverCertError(long id, int status, String url, String pem) {
        final WebView2Host host = mHost;
        if (host == null) return;
        int primary;
        switch (status) {
            case 1: primary = SslError.SSL_IDMISMATCH; break;  // COMMON_NAME_IS_INCORRECT
            case 2: primary = SslError.SSL_EXPIRED; break;     // EXPIRED
            case 4:                                            // REVOKED
            case 3: primary = SslError.SSL_INVALID; break;     // CLIENT_CERTIFICATE_CONTAINS_ERRORS
            default: primary = SslError.SSL_UNTRUSTED; break;  // IS_INVALID and the rest
        }
        SslCertificate certificate = null;
        try {
            X509Certificate x509 = (X509Certificate) CertificateFactory.getInstance("X.509")
                    .generateCertificate(new ByteArrayInputStream(pemToDer(pem)));
            certificate = new SslCertificate(x509);
        } catch (Exception | LinkageError e) {
            GesherWebView.log("certificate error: the certificate does not parse (" + e + ")");
        }
        String appUrl = FileUrls.fromWebView2(url);
        SslError error = certificate != null ? new SslError(primary, certificate, appUrl)
                : new SslError(primary, new SslCertificate("", "", "", ""), appUrl);
        final AtomicBoolean done = new AtomicBoolean();
        SslErrorHandler handler = new SslErrorHandler() {
            @Override
            public void proceed() {
                if (done.compareAndSet(false, true)) host.completeServerCertError(id, true);
            }

            @Override
            public void cancel() {
                if (done.compareAndSet(false, true)) host.completeServerCertError(id, false);
            }
        };
        mClient.onReceivedSslError(mView, handler, error);
    }

    /** The page's height in CSS pixels, for WRAP_CONTENT heights (getContentHeight). */
    private void measureContent() {
        evaluateJavaScript("Math.max(document.documentElement ? document.documentElement.scrollHeight"
                + " : 0, document.body ? document.body.scrollHeight : 0)", value -> {
            try {
                int h = (int) Double.parseDouble(value);
                if (h != mContentHeight) {
                    mContentHeight = h;
                    mView.requestLayout();
                }
            } catch (RuntimeException e) {
                // not a number
            }
        });
    }

    /** $GESHER_WEBVIEW_DEBUG: a picture of the page (CapturePreview), summarized. */
    private void capturePreviewForLog(String url) {
        mUi.postDelayed(() -> {
            if (mHost == null) return;
            long cb = mNextCallback.getAndIncrement();
            mScriptCallbacks.put(cb, base64 -> {
                if (base64 == null) {
                    GesherWebView.log("preview of " + url + ": none");
                    return;
                }
                byte[] png = Base64.decode(base64, Base64.DEFAULT);
                Bitmap bmp = BitmapFactory.decodeByteArray(png, 0, png.length);
                if (bmp == null) {
                    GesherWebView.log("preview of " + url + ": " + png.length + " bytes, undecodable");
                    return;
                }
                int center = bmp.getPixel(bmp.getWidth() / 2, bmp.getHeight() / 2) & 0xffffff;
                Map<Integer, Integer> counts = new HashMap<>();
                int light = 0;
                for (int y = 0; y < bmp.getHeight(); y += 4) {
                    for (int x = 0; x < bmp.getWidth(); x += 4) {
                        int p = bmp.getPixel(x, y) & 0xffffff;
                        counts.merge(p, 1, Integer::sum);
                        if (((p >> 16) & 0xff) > 200 && ((p >> 8) & 0xff) > 200 && (p & 0xff) > 200) {
                            light++;
                        }
                    }
                }
                int most = 0;
                int mostColor = 0;
                for (Map.Entry<Integer, Integer> e : counts.entrySet()) {
                    if (e.getValue() > most) {
                        most = e.getValue();
                        mostColor = e.getKey();
                    }
                }
                GesherWebView.log(String.format("preview of %s: %dx%d, center #%06x, most #%06x"
                        + " (%d of %d samples), %d light", url, bmp.getWidth(), bmp.getHeight(),
                        center, mostColor, most, ((bmp.getWidth() + 3) / 4)
                                * ((bmp.getHeight() + 3) / 4), light));
            });
            mHost.capturePreview(cb);
        }, 500);
    }

    private void console(String event, String json) {
        String message;
        String source = "";
        int line = 0;
        ConsoleMessage.MessageLevel level = ConsoleMessage.MessageLevel.LOG;
        try {
            JSONObject p = new JSONObject(json);
            if ("Runtime.exceptionThrown".equals(event)) {
                JSONObject details = p.getJSONObject("exceptionDetails");
                JSONObject exception = details.optJSONObject("exception");
                message = "Uncaught " + (exception != null
                        ? exception.optString("description", details.optString("text"))
                        : details.optString("text"));
                source = details.optString("url");
                line = details.optInt("lineNumber") + 1;
                level = ConsoleMessage.MessageLevel.ERROR;
            } else {
                StringBuilder text = new StringBuilder();
                JSONArray args = p.optJSONArray("args");
                for (int i = 0; args != null && i < args.length(); i++) {
                    JSONObject arg = args.getJSONObject(i);
                    if (text.length() > 0) text.append(' ');
                    if (arg.has("value")) {
                        text.append(String.valueOf(arg.get("value")));
                    } else {
                        text.append(arg.optString("description", arg.optString("type")));
                    }
                }
                message = text.toString();
                switch (p.optString("type")) {
                    case "error":
                    case "assert":
                        level = ConsoleMessage.MessageLevel.ERROR;
                        break;
                    case "warning":
                        level = ConsoleMessage.MessageLevel.WARNING;
                        break;
                    case "debug":
                        level = ConsoleMessage.MessageLevel.DEBUG;
                        break;
                    default:
                        level = ConsoleMessage.MessageLevel.LOG;
                }
                JSONObject stack = p.optJSONObject("stackTrace");
                JSONArray frames = stack != null ? stack.optJSONArray("callFrames") : null;
                if (frames != null && frames.length() > 0) {
                    JSONObject f = frames.getJSONObject(0);
                    source = f.optString("url");
                    line = f.optInt("lineNumber") + 1;
                }
            }
        } catch (Exception e) {
            message = json;
        }
        source = FileUrls.fromWebView2(source);
        ConsoleMessage cm = new ConsoleMessage(message, source, line, level);
        WebChromeClient chrome = mChrome;
        if (chrome == null || !chrome.onConsoleMessage(cm)) {
            Log.i("chromium", "[INFO:CONSOLE(" + line + ")] \"" + message + "\", source: "
                    + source + " (" + line + ")");
        }
    }

    private void showDialog(long id, int kind, String url, String message, String defaultText) {
        final WebView2Host host = mHost;
        JsPromptResult result = new JsPromptResult(r -> {
            if (host == null) return;
            String text = r instanceof JsPromptResult ? ((JsPromptResult) r).getStringResult()
                    : null;
            host.completeDialog(id, r.getResult(), text != null ? text : "");
        });
        JsDialogHelper helper = new JsDialogHelper(result, kind, defaultText, message,
                FileUrls.fromWebView2(url));
        WebChromeClient chrome = mChrome;
        if (chrome == null || !helper.invokeCallback(chrome, mView)) {
            helper.showDialog(mContext);
        }
    }

    private static int errorCode(int status) {
        switch (status) {
            case 1: // CERTIFICATE_COMMON_NAME_IS_INCORRECT
            case 2: // CERTIFICATE_EXPIRED
            case 3: // CLIENT_CERTIFICATE_CONTAINS_ERRORS
            case 4: // CERTIFICATE_REVOKED
            case 5: // CERTIFICATE_IS_INVALID
                return WebViewClient.ERROR_FAILED_SSL_HANDSHAKE;
            case 6: // SERVER_UNREACHABLE
            case 9: // CONNECTION_ABORTED
            case 10: // CONNECTION_RESET
            case 11: // DISCONNECTED
            case 12: // CANNOT_CONNECT
                return WebViewClient.ERROR_CONNECT;
            case 7: // TIMEOUT
                return WebViewClient.ERROR_TIMEOUT;
            case 8: // ERROR_HTTP_INVALID_SERVER_RESPONSE
                return WebViewClient.ERROR_IO;
            case 13: // HOST_NAME_NOT_RESOLVED
                return WebViewClient.ERROR_HOST_LOOKUP;
            case 15: // REDIRECT_FAILED
                return WebViewClient.ERROR_REDIRECT_LOOP;
            case 17: // VALID_AUTHENTICATION_CREDENTIALS_REQUIRED
                return WebViewClient.ERROR_AUTHENTICATION;
            case 18: // VALID_PROXY_AUTHENTICATION_REQUIRED
                return WebViewClient.ERROR_PROXY_AUTHENTICATION;
            default:
                return WebViewClient.ERROR_UNKNOWN;
        }
    }

    private static String errorName(int status) {
        String[] names = {"UNKNOWN", "CERT_COMMON_NAME_INVALID", "CERT_DATE_INVALID",
                "BAD_SSL_CLIENT_AUTH_CERT", "CERT_REVOKED", "CERT_INVALID", "ADDRESS_UNREACHABLE",
                "TIMED_OUT", "INVALID_HTTP_RESPONSE", "CONNECTION_ABORTED", "CONNECTION_RESET",
                "INTERNET_DISCONNECTED", "CONNECTION_FAILED", "NAME_NOT_RESOLVED", "ABORTED",
                "TOO_MANY_REDIRECTS", "UNEXPECTED", "AUTH_REQUIRED", "PROXY_AUTH_REQUIRED"};
        return status >= 0 && status < names.length ? "ERR_" + names[status] : "ERR_FAILED";
    }

    /** A WebResourceRequest for the app's callbacks. */
    private static final class Request implements WebResourceRequest {
        private final Uri mUrl;
        private final boolean mMainFrame;
        private final boolean mRedirect;
        private final boolean mGesture;
        private final String mMethod;
        private final Map<String, String> mHeaders;

        Request(String url, boolean mainFrame, boolean redirect, boolean gesture, String method,
                Map<String, String> headers) {
            mUrl = Uri.parse(url != null ? url : "");
            mMainFrame = mainFrame;
            mRedirect = redirect;
            mGesture = gesture;
            mMethod = method != null && !method.isEmpty() ? method : "GET";
            mHeaders = headers != null ? headers : Collections.emptyMap();
        }

        @Override
        public Uri getUrl() {
            return mUrl;
        }

        @Override
        public boolean isForMainFrame() {
            return mMainFrame;
        }

        @Override
        public boolean isRedirect() {
            return mRedirect;
        }

        @Override
        public boolean hasGesture() {
            return mGesture;
        }

        @Override
        public String getMethod() {
            return mMethod;
        }

        @Override
        public Map<String, String> getRequestHeaders() {
            return mHeaders;
        }
    }
}
