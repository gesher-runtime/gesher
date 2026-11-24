package com.gesher.app.webview;

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
import android.webkit.WebStorage;
import android.webkit.WebView;
import android.webkit.WebViewDatabase;
import android.webkit.WebViewFactoryProvider;
import android.webkit.WebViewProvider;

import java.io.OutputStream;
import java.util.List;
import java.util.concurrent.Executor;

/**
 * Gesher's WebViewFactoryProvider (docs/webview.md): what WebViewFactory.getProvider()
 * returns once GesherWebView.install() ran. Its WebViews are WebView2 controllers.
 */
final class GesherWebViewFactory implements WebViewFactoryProvider {
    private final Object mLock = new Object();
    private GesherCookieManager mCookies;
    private GesherWebStorage mStorage;
    private GeolocationPermissions mGeolocation;
    private ServiceWorkerController mServiceWorkers;
    private TracingController mTracing;
    private WebViewDatabase mDatabase;
    private final Statics mStatics = new GesherStatics();

    @Override
    public Statics getStatics() {
        return mStatics;
    }

    @Override
    public WebViewProvider createWebView(WebView webView, WebView.PrivateAccess privateAccess) {
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
        synchronized (mLock) {
            if (mServiceWorkers == null) mServiceWorkers = new GesherServiceWorkers();
            return mServiceWorkers;
        }
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

    private static final class GesherServiceWorkers extends ServiceWorkerController {
        private final ServiceWorkerWebSettings mSettings = new ServiceWorkerWebSettings() {
            private int mCacheMode;
            private boolean mContent = true;
            private boolean mFile;
            private boolean mBlock;

            @Override
            public void setCacheMode(int mode) {
                mCacheMode = mode;
            }

            @Override
            public int getCacheMode() {
                return mCacheMode;
            }

            @Override
            public void setAllowContentAccess(boolean allow) {
                mContent = allow;
            }

            @Override
            public boolean getAllowContentAccess() {
                return mContent;
            }

            @Override
            public void setAllowFileAccess(boolean allow) {
                mFile = allow;
            }

            @Override
            public boolean getAllowFileAccess() {
                return mFile;
            }

            @Override
            public void setBlockNetworkLoads(boolean flag) {
                mBlock = flag;
            }

            @Override
            public boolean getBlockNetworkLoads() {
                return mBlock;
            }
        };

        @Override
        public ServiceWorkerWebSettings getServiceWorkerWebSettings() {
            return mSettings;
        }

        @Override
        public void setServiceWorkerClient(ServiceWorkerClient client) {
        }
    }
}
