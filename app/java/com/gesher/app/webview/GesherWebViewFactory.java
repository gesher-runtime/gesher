package com.gesher.app.webview;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.webkit.CookieManager;
import android.webkit.GeolocationPermissions;
import android.webkit.ServiceWorkerClient;
import android.webkit.ServiceWorkerController;
import android.webkit.ServiceWorkerWebSettings;
import android.webkit.TokenBindingService;
import android.webkit.TracingConfig;
import android.webkit.TracingController;
import android.webkit.ValueCallback;
import android.webkit.WebIconDatabase;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebStorage;
import android.webkit.WebView;
import android.webkit.WebViewDatabase;
import android.webkit.WebViewFactoryProvider;
import android.webkit.WebViewProvider;

import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.CacheResponse;
import java.net.ResponseCache;
import java.net.URI;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.Executor;

/**
 * Gesher's WebViewFactoryProvider (docs/webview.md): what WebViewFactory.getProvider()
 * returns once GesherWebView.install() ran. Its WebViews are WebView2 controllers.
 */
final class GesherWebViewFactory implements WebViewFactoryProvider {
    private static final Object sServiceWorkersLock = new Object();
    private static GesherServiceWorkers sServiceWorkers;

    private final Object mLock = new Object();
    private GesherCookieManager mCookies;
    private GesherWebStorage mStorage;
    private GeolocationPermissions mGeolocation;
    private TracingController mTracing;
    private WebViewDatabase mDatabase;
    private final Statics mStatics = new GesherStatics();

    static GesherServiceWorkers serviceWorkers() {
        synchronized (sServiceWorkersLock) {
            if (sServiceWorkers == null) sServiceWorkers = new GesherServiceWorkers();
            return sServiceWorkers;
        }
    }

    @Override
    public Statics getStatics() {
        return mStatics;
    }

    @Override
    public WebViewProvider createWebView(WebView webView, WebView.PrivateAccess privateAccess) {
        if (webView != null && webView.getContext() != null) {
            serviceWorkers().attachContext(webView.getContext());
        }
        return new GesherWebViewProvider(webView, privateAccess);
    }

    @Override
    public GeolocationPermissions getGeolocationPermissions() {
        synchronized (mLock) {
            if (mGeolocation == null) mGeolocation = new GesherGeolocationPermissions();
            return mGeolocation;
        }
    }

    @Override
    public CookieManager getCookieManager() {
        synchronized (mLock) {
            if (mCookies == null) mCookies = new GesherCookieManager();
            return mCookies;
        }
    }

    @Override
    public TokenBindingService getTokenBindingService() {
        return null;
    }

    @Override
    public TracingController getTracingController() {
        synchronized (mLock) {
            if (mTracing == null) {
                mTracing = new TracingController() {
                    @Override
                    public void start(TracingConfig tracingConfig) {
                    }

                    @Override
                    public boolean stop(OutputStream outputStream, Executor executor) {
                        return false;
                    }

                    @Override
                    public boolean isTracing() {
                        return false;
                    }
                };
            }
            return mTracing;
        }
    }

    @Override
    public ServiceWorkerController getServiceWorkerController() {
        return serviceWorkers();
    }

    @Override
    public WebIconDatabase getWebIconDatabase() {
        return null;
    }

    @Override
    public WebStorage getWebStorage() {
        synchronized (mLock) {
            if (mStorage == null) mStorage = new GesherWebStorage();
            return mStorage;
        }
    }

    @Override
    public WebViewDatabase getWebViewDatabase(Context context) {
        if (context != null) serviceWorkers().attachContext(context);
        synchronized (mLock) {
            if (mDatabase == null) {
                mDatabase = new WebViewDatabase() {
                    @Override
                    public boolean hasUsernamePassword() {
                        return false;
                    }

                    @Override
                    public void clearUsernamePassword() {
                    }

                    @Override
                    public boolean hasHttpAuthUsernamePassword() {
                        return false;
                    }

                    @Override
                    public void clearHttpAuthUsernamePassword() {
                    }

                    @Override
                    public void setHttpAuthUsernamePassword(String host, String realm,
                            String username, String password) {
                    }

                    @Override
                    public String[] getHttpAuthUsernamePassword(String host, String realm) {
                        return null;
                    }

                    @Override
                    public boolean hasFormData() {
                        return false;
                    }

                    @Override
                    public void clearFormData() {
                    }
                };
            }
            return mDatabase;
        }
    }

    @Override
    public ClassLoader getWebViewClassLoader() {
        return GesherWebViewFactory.class.getClassLoader();
    }

    private static final class GesherStatics implements Statics {
        @Override
        public String findAddress(String addr) {
            return null;
        }

        @Override
        public String getDefaultUserAgent(Context context) {
            return GesherWebView.defaultUserAgent();
        }

        @Override
        public void freeMemoryForTests() {
        }

        @Override
        public void setWebContentsDebuggingEnabled(boolean enable) {
            GesherWebViewProvider.sDevTools = enable;
        }

        @Override
        public void clearClientCertPreferences(Runnable onCleared) {
            if (onCleared != null) onCleared.run();
        }

        @Override
        public void enableSlowWholeDocumentDraw() {
        }

        @Override
        public Uri[] parseFileChooserResult(int resultCode, Intent intent) {
            if (intent == null || resultCode != android.app.Activity.RESULT_OK) return null;
            Uri data = intent.getData();
            return data != null ? new Uri[] {data} : null;
        }

        @Override
        public void initSafeBrowsing(Context context, ValueCallback<Boolean> callback) {
            // WebSettings' Safe Browsing is WebView2's SmartScreen (reputation checking),
            // which needs no initialization.
            if (callback != null) callback.onReceiveValue(true);
        }

        @Override
        public void setSafeBrowsingWhitelist(List<String> hosts, ValueCallback<Boolean> callback) {
            if (callback != null) callback.onReceiveValue(true);
        }

        @Override
        public Uri getSafeBrowsingPrivacyPolicyUrl() {
            return Uri.parse("https://www.microsoft.com/privacy");
        }
    }

    static final class GesherServiceWorkers extends ServiceWorkerController {
        private final ServiceWorkerWebSettings mSettings = new Settings();
        private volatile ServiceWorkerClient mClient;
        private volatile Context mContext;

        static final class Settings extends ServiceWorkerWebSettings {
            private int mCacheMode = WebSettings.LOAD_DEFAULT;
            private boolean mContent = true;
            private boolean mFile = false;
            private boolean mBlock = false;

            @Override
            public synchronized void setCacheMode(int mode) {
                mCacheMode = mode;
            }

            @Override
            public synchronized int getCacheMode() {
                return mCacheMode;
            }

            @Override
            public synchronized void setAllowContentAccess(boolean allow) {
                mContent = allow;
            }

            @Override
            public synchronized boolean getAllowContentAccess() {
                return mContent;
            }

            @Override
            public synchronized void setAllowFileAccess(boolean allow) {
                mFile = allow;
            }

            @Override
            public synchronized boolean getAllowFileAccess() {
                return mFile;
            }

            @Override
            public synchronized void setBlockNetworkLoads(boolean flag) {
                mBlock = flag;
            }

            @Override
            public synchronized boolean getBlockNetworkLoads() {
                return mBlock;
            }
        }

        private static final class SwRequest implements WebResourceRequest {
            private final Uri mUrl;
            private final boolean mRedirect;
            private final String mMethod;
            private final Map<String, String> mHeaders;

            SwRequest(Uri url, boolean redirect, String method, Map<String, String> headers) {
                mUrl = url != null ? url : Uri.EMPTY;
                mRedirect = redirect;
                mMethod = method != null && !method.isEmpty() ? method : "GET";
                mHeaders = headers != null
                        ? Collections.unmodifiableMap(new LinkedHashMap<>(headers))
                        : Collections.emptyMap();
            }

            @Override
            public Uri getUrl() {
                return mUrl;
            }

            @Override
            public boolean isForMainFrame() {
                return false;
            }

            @Override
            public boolean isRedirect() {
                return mRedirect;
            }

            @Override
            public boolean hasGesture() {
                return false;
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

        void attachContext(Context context) {
            if (context == null) return;
            Context app = context.getApplicationContext();
            mContext = app != null ? app : context;
        }

        @Override
        public ServiceWorkerWebSettings getServiceWorkerWebSettings() {
            return mSettings;
        }

        @Override
        public void setServiceWorkerClient(ServiceWorkerClient client) {
            mClient = client;
        }

        ServiceWorkerClient getServiceWorkerClient() {
            return mClient;
        }

        /** Evaluates ServiceWorkerClient interception and ServiceWorkerWebSettings policy. */
        WebResourceResponse shouldInterceptRequest(WebResourceRequest request) {
            return intercept(mContext, request);
        }

        WebResourceResponse intercept(Context context, String url, String method,
                Map<String, String> headers) {
            String appUrl = FileUrls.fromWebView2(url != null ? url : "");
            return intercept(context != null ? context : mContext,
                    new SwRequest(Uri.parse(appUrl), false, method, headers));
        }

        WebResourceResponse intercept(Context context, WebResourceRequest request) {
            Objects.requireNonNull(request, "request");
            Context effectiveContext = context != null ? context : mContext;
            Uri rawUri = request.getUrl();
            String rawUrl = rawUri != null ? rawUri.toString() : "";
            String appUrl = FileUrls.fromWebView2(rawUrl);
            WebResourceRequest effectiveRequest = !appUrl.equals(rawUrl) || request.isForMainFrame()
                    ? new SwRequest(Uri.parse(appUrl), request.isRedirect(),
                            request.getMethod(), request.getRequestHeaders())
                    : request;

            ServiceWorkerClient client = mClient;
            if (client != null) {
                WebResourceResponse intercepted = client.shouldInterceptRequest(effectiveRequest);
                if (intercepted != null) return intercepted;
            }

            Uri uri = effectiveRequest.getUrl();
            String scheme = uri != null && uri.getScheme() != null
                    ? uri.getScheme().toLowerCase(Locale.ROOT) : "";
            if ("content".equals(scheme)) {
                if (!mSettings.getAllowContentAccess()) {
                    return statusResponse("text/plain", 403, "Blocked", new byte[0]);
                }
                ContentResolver resolver = effectiveContext != null
                        ? effectiveContext.getContentResolver() : null;
                if (resolver == null) {
                    return statusResponse("text/plain", 404, "Not Found", new byte[0]);
                }
                try {
                    InputStream stream = resolver.openInputStream(uri);
                    if (stream == null) {
                        return statusResponse("text/plain", 404, "Not Found", new byte[0]);
                    }
                    String mime = resolver.getType(uri);
                    if (mime == null || mime.isEmpty()) {
                        mime = FileUrls.mimeType(uri.getPath() != null ? uri.getPath() : "");
                    }
                    return streamResponse(mime, 200, "OK", stream);
                } catch (Exception e) {
                    return statusResponse("text/plain", 404, "Not Found", new byte[0]);
                }
            }

            if ("file".equals(scheme) || FileUrls.isFile(rawUrl)) {
                return FileUrls.load(effectiveContext, appUrl,
                        mSettings.getAllowFileAccess() || appUrl.startsWith("file:///android_"));
            }

            if ("http".equals(scheme) || "https".equals(scheme)) {
                int cacheMode = mSettings.getCacheMode();
                if (cacheMode == WebSettings.LOAD_CACHE_ONLY) {
                    WebResourceResponse cached = fromHttpCache(effectiveRequest);
                    return cached != null ? cached
                            : statusResponse("text/plain", 504,
                                    "Unsatisfiable Request (only-if-cached)", new byte[0]);
                }
                if (cacheMode == WebSettings.LOAD_CACHE_ELSE_NETWORK) {
                    WebResourceResponse cached = fromHttpCache(effectiveRequest);
                    if (cached != null) return cached;
                }
                if (mSettings.getBlockNetworkLoads()) {
                    return statusResponse("text/plain", 403, "Blocked", new byte[0]);
                }
            }
            return null;
        }

        Map<String, String> effectiveRequestHeaders(Map<String, String> headers) {
            Map<String, String> out = new LinkedHashMap<>();
            if (headers != null) out.putAll(headers);
            switch (mSettings.getCacheMode()) {
                case WebSettings.LOAD_NO_CACHE:
                    out.put("Cache-Control", "no-cache");
                    out.put("Pragma", "no-cache");
                    break;
                case WebSettings.LOAD_CACHE_ELSE_NETWORK:
                    out.put("Cache-Control", "max-stale=2147483647");
                    break;
                case WebSettings.LOAD_CACHE_ONLY:
                    out.put("Cache-Control", "only-if-cached");
                    break;
                default:
                    break;
            }
            return out;
        }

        private static WebResourceResponse fromHttpCache(WebResourceRequest request) {
            String method = request.getMethod();
            if (method != null && !"GET".equalsIgnoreCase(method)
                    && !"HEAD".equalsIgnoreCase(method)) {
                return null;
            }
            ResponseCache cache = ResponseCache.getDefault();
            if (cache == null || request.getUrl() == null) return null;
            try {
                Map<String, List<String>> reqHeaders = new LinkedHashMap<>();
                for (Map.Entry<String, String> entry : request.getRequestHeaders().entrySet()) {
                    reqHeaders.put(entry.getKey(), Collections.singletonList(entry.getValue()));
                }
                CacheResponse hit = cache.get(URI.create(request.getUrl().toString()),
                        method != null ? method : "GET", reqHeaders);
                if (hit == null) return null;
                InputStream body = hit.getBody();
                if (body == null) return null;
                Map<String, String> flatHeaders = new LinkedHashMap<>();
                String contentType = null;
                Map<String, List<String>> rawHeaders = hit.getHeaders();
                if (rawHeaders != null) {
                    for (Map.Entry<String, List<String>> e : rawHeaders.entrySet()) {
                        if (e.getKey() == null || e.getValue() == null || e.getValue().isEmpty()) {
                            continue;
                        }
                        String value = String.join(", ", e.getValue());
                        flatHeaders.put(e.getKey(), value);
                        if ("content-type".equalsIgnoreCase(e.getKey())) contentType = value;
                    }
                }
                String mime = "application/octet-stream";
                String encoding = null;
                if (contentType != null) {
                    int semi = contentType.indexOf(';');
                    mime = (semi >= 0 ? contentType.substring(0, semi) : contentType).trim();
                    if (semi >= 0) {
                        String rest = contentType.substring(semi + 1).trim();
                        if (rest.toLowerCase(Locale.ROOT).startsWith("charset=")) {
                            encoding = rest.substring("charset=".length()).trim();
                        }
                    }
                }
                return new WebResourceResponse(mime, encoding, 200, "OK", flatHeaders, body);
            } catch (Exception e) {
                return null;
            }
        }

        private static WebResourceResponse statusResponse(String mime, int status, String reason,
                byte[] body) {
            return streamResponse(mime, status, reason, new ByteArrayInputStream(body));
        }

        private static WebResourceResponse streamResponse(String mime, int status, String reason,
                InputStream stream) {
            Map<String, String> headers = new HashMap<>();
            headers.put("Access-Control-Allow-Origin", "*");
            headers.put("Cache-Control", "no-cache");
            return new WebResourceResponse(mime, null, status, reason, headers, stream);
        }
    }
}
