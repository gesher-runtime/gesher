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
import android.webkit.ConsoleMessage;
import android.webkit.DownloadListener;
import android.webkit.JsDialogHelper;
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
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
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
                s.getUserAgentString());
        // Text zoom has no WebView2 equivalent: the page zooms as a whole.
        mHost.setZoom(Math.max(0.25, Math.min(5.0, s.getTextZoom() / 100.0)));
        updateDocumentScript();
    }

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
        script.append(JsBridge.script(mInterfaces, mRequireAnnotation));
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
    }

    @Override
    public String[] getHttpAuthUsernamePassword(String host, String realm) {
        return null;
    }

    @Override
    public void destroy() {
        if (mDestroyed) return;
        mDestroyed = true;
        removeListeners();
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
        // WebView2's NavigateWithWebResourceRequest takes a body stream; Gesher's host
        // navigates with GET only: the URL alone.
        loadUrl(url);
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
    }

    @Override
    public void saveWebArchive(String basename, boolean autoname, ValueCallback<String> callback) {
        if (callback != null) mUi.post(() -> callback.onReceiveValue(null));
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
        return steps == 0 || (steps == -1 && mCanGoBack) || (steps == 1 && mCanGoForward);
    }

    @Override
    public void goBackOrForward(int steps) {
        if (steps < 0) {
            for (int i = 0; i < -steps; i++) goBack();
        } else {
            for (int i = 0; i < steps; i++) goForward();
        }
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

    @Override
    public Picture capturePicture() {
        return new Picture();
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
    }

    @Override
    public void clearSslPreferences() {
    }

    @Override
    public WebBackForwardList copyBackForwardList() {
        return null;
    }

    @Override
    public void setFindListener(WebView.FindListener listener) {
    }

    @Override
    public void findNext(boolean forward) {
    }

    @Override
    public int findAll(String find) {
        return 0;
    }

    @Override
    public void findAllAsync(String find) {
    }

    @Override
    public boolean showFindDialog(String text, boolean showIme) {
        return false;
    }

    @Override
    public void clearMatches() {
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
        throw new UnsupportedOperationException("WebMessagePort is not supported on Gesher yet");
    }

    @Override
    public void postMessageToMainFrame(WebMessage message, Uri targetOrigin) {
        if (message == null) return;
        String data = message.getData();
        evaluateJavaScript("window.dispatchEvent(new MessageEvent('message', {data: "
                + JSONObject.quote(data != null ? data : "") + "}))", null);
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
        if (mPlacedOnce && Arrays.equals(now, mPlaced)) return;
        System.arraycopy(now, 0, mPlaced, 0, now.length);
        mPlacedOnce = true;
        mHost.setBounds(hwnd, mLocation[0], mLocation[1], w, h, mClip.left, mClip.top,
                mClip.width(), mClip.height(), visible);
        if (GesherWebView.DEBUG) {
            GesherWebView.log("bounds: hwnd 0x" + Long.toHexString(hwnd) + " view "
                    + mLocation[0] + "," + mLocation[1] + " " + w + "x" + h + ", visible "
                    + (visible ? mClip.toShortString() : "no"));
        }
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
        return null;
    }

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
                // Without onCreateWindow support, the page opens in this WebView, as
                // Android's does when multiple windows are off.
                if (a != null && !a.isEmpty()) loadUrl(FileUrls.fromWebView2(a));
                break;
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
