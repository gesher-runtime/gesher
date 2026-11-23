#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Contract tests for W07/W08: SupportLib ServiceWorkerController, ServiceWorkerWebSettings,
ServiceWorkerClient.shouldInterceptRequest, and GesherWebStorage.

Compiles the real production SupportLib.java, GesherWebViewFactory.java,
GesherWebStorage.java, FileUrls.java and WebMessagePayload.java on a host JDK.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

STUBS: dict[str, str] = {
    # Chromium support_lib_boundary interfaces (app classloader side)
    "org/chromium/support_lib_boundary/WebViewProviderFactoryBoundaryInterface.java": """package org.chromium.support_lib_boundary;
import android.webkit.WebView;
import java.lang.reflect.InvocationHandler;
public interface WebViewProviderFactoryBoundaryInterface {
    InvocationHandler createWebView(WebView webview);
    InvocationHandler getWebkitToCompatConverter();
    InvocationHandler getStatics();
    String[] getSupportedFeatures();
    InvocationHandler getServiceWorkerController();
    InvocationHandler getTracingController();
    InvocationHandler getProxyController();
}
""",
    "org/chromium/support_lib_boundary/WebkitToCompatConverterBoundaryInterface.java": """package org.chromium.support_lib_boundary;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import java.lang.reflect.InvocationHandler;
public interface WebkitToCompatConverterBoundaryInterface {
    InvocationHandler convertSettings(WebSettings webSettings);
    InvocationHandler convertWebResourceRequest(WebResourceRequest request);
    InvocationHandler convertServiceWorkerSettings(Object serviceWorkerWebSettings);
    Object convertServiceWorkerSettings(InvocationHandler serviceWorkerSettings);
    InvocationHandler convertWebMessagePort(Object webMessagePort);
    Object convertWebMessagePort(InvocationHandler webMessagePort);
}
""",
    "org/chromium/support_lib_boundary/ServiceWorkerControllerBoundaryInterface.java": """package org.chromium.support_lib_boundary;
import java.lang.reflect.InvocationHandler;
public interface ServiceWorkerControllerBoundaryInterface {
    InvocationHandler getServiceWorkerWebSettings();
    void setServiceWorkerClient(InvocationHandler client);
}
""",
    "org/chromium/support_lib_boundary/ServiceWorkerWebSettingsBoundaryInterface.java": """package org.chromium.support_lib_boundary;
public interface ServiceWorkerWebSettingsBoundaryInterface {
    void setCacheMode(int mode);
    int getCacheMode();
    void setAllowContentAccess(boolean allow);
    boolean getAllowContentAccess();
    void setAllowFileAccess(boolean allow);
    boolean getAllowFileAccess();
    void setBlockNetworkLoads(boolean flag);
    boolean getBlockNetworkLoads();
}
""",
    "org/chromium/support_lib_boundary/ServiceWorkerClientBoundaryInterface.java": """package org.chromium.support_lib_boundary;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
public interface ServiceWorkerClientBoundaryInterface {
    WebResourceResponse shouldInterceptRequest(WebResourceRequest request);
    String[] getSupportedFeatures();
}
""",
    # Minimal Android stubs for compiling real production WebView classes
    "android/app/Activity.java": """package android.app;
public class Activity {
    public static final int RESULT_OK = -1;
}
""",
    "android/os/Handler.java": """package android.os;
public class Handler {}
""",
    "android/util/Log.java": """package android.util;
public final class Log {
    public static int e(String tag, String msg, Throwable tr) { return 0; }
}
""",
    "android/util/TypedValue.java": """package android.util;
public class TypedValue {}
""",
    "android/util/Base64.java": """package android.util;
public final class Base64 {
    public static final int DEFAULT = 0;
    public static final int NO_WRAP = 2;
    public static byte[] decode(String str, int flags) {
        return java.util.Base64.getDecoder().decode(str);
    }
    public static String encodeToString(byte[] input, int flags) {
        return java.util.Base64.getEncoder().encodeToString(input);
    }
}
""",
    "android/net/Uri.java": """package android.net;
import java.net.URI;
import java.util.Objects;
public final class Uri {
    public static final Uri EMPTY = new Uri("");
    private final String mRaw;
    private Uri(String raw) { mRaw = raw == null ? "" : raw; }
    public static Uri parse(String uriString) {
        return uriString == null || uriString.isEmpty() ? EMPTY : new Uri(uriString);
    }
    public String getScheme() {
        int colon = mRaw.indexOf(':');
        return colon > 0 ? mRaw.substring(0, colon) : null;
    }
    public String getPath() {
        try { return new URI(mRaw).getPath(); }
        catch (Exception e) {
            int s = mRaw.indexOf("://");
            if (s < 0) return null;
            int slash = mRaw.indexOf('/', s + 3);
            return slash >= 0 ? mRaw.substring(slash) : "/";
        }
    }
    @Override public String toString() { return mRaw; }
    @Override public boolean equals(Object o) { return o instanceof Uri && Objects.equals(mRaw, ((Uri) o).mRaw); }
    @Override public int hashCode() { return mRaw.hashCode(); }
}
""",
    "android/content/Intent.java": """package android.content;
import android.net.Uri;
public class Intent {
    private final Uri mData;
    public Intent() { this(null); }
    public Intent(Uri data) { mData = data; }
    public Uri getData() { return mData; }
}
""",
    "android/content/ContentResolver.java": """package android.content;
import android.net.Uri;
import java.io.IOException;
import java.io.InputStream;
public abstract class ContentResolver {
    public abstract InputStream openInputStream(Uri uri) throws IOException;
    public abstract String getType(Uri uri);
}
""",
    "android/content/res/AssetManager.java": """package android.content.res;
import java.io.IOException;
import java.io.InputStream;
public abstract class AssetManager {
    public abstract InputStream open(String fileName) throws IOException;
}
""",
    "android/content/res/Resources.java": """package android.content.res;
import android.util.TypedValue;
import java.io.InputStream;
public abstract class Resources {
    public abstract int getIdentifier(String name, String defType, String defPackage);
    public abstract InputStream openRawResource(int id, TypedValue value);
}
""",
    "android/content/Context.java": """package android.content;
import android.content.res.AssetManager;
import android.content.res.Resources;
public abstract class Context {
    public Context getApplicationContext() { return this; }
    public ContentResolver getContentResolver() { return null; }
    public AssetManager getAssets() { return null; }
    public Resources getResources() { return null; }
    public String getPackageName() { return "com.gesher.test"; }
}
""",
    "android/webkit/ValueCallback.java": """package android.webkit;
public interface ValueCallback<T> {
    void onReceiveValue(T value);
}
""",
    "android/webkit/MimeTypeMap.java": """package android.webkit;
public final class MimeTypeMap {
    private static final MimeTypeMap INSTANCE = new MimeTypeMap();
    public static MimeTypeMap getSingleton() { return INSTANCE; }
    public String getMimeTypeFromExtension(String ext) { return null; }
}
""",
    "android/webkit/WebResourceRequest.java": """package android.webkit;
import android.net.Uri;
import java.util.Map;
public interface WebResourceRequest {
    Uri getUrl();
    boolean isForMainFrame();
    boolean isRedirect();
    boolean hasGesture();
    String getMethod();
    Map<String, String> getRequestHeaders();
}
""",
    "android/webkit/WebResourceResponse.java": """package android.webkit;
import java.io.InputStream;
import java.util.Map;
public class WebResourceResponse {
    private final String mMimeType;
    private final String mEncoding;
    private final int mStatusCode;
    private final String mReasonPhrase;
    private final Map<String, String> mResponseHeaders;
    private final InputStream mInputStream;
    public WebResourceResponse(String mimeType, String encoding, InputStream data) {
        this(mimeType, encoding, 200, "OK", null, data);
    }
    public WebResourceResponse(String mimeType, String encoding, int statusCode,
            String reasonPhrase, Map<String, String> responseHeaders, InputStream data) {
        mMimeType = mimeType;
        mEncoding = encoding;
        mStatusCode = statusCode;
        mReasonPhrase = reasonPhrase;
        mResponseHeaders = responseHeaders;
        mInputStream = data;
    }
    public String getMimeType() { return mMimeType; }
    public String getEncoding() { return mEncoding; }
    public int getStatusCode() { return mStatusCode; }
    public String getReasonPhrase() { return mReasonPhrase; }
    public Map<String, String> getResponseHeaders() { return mResponseHeaders; }
    public InputStream getData() { return mInputStream; }
}
""",
    "android/webkit/WebSettings.java": """package android.webkit;
public abstract class WebSettings {
    public static final int LOAD_DEFAULT = -1;
    public static final int LOAD_NORMAL = 0;
    public static final int LOAD_CACHE_ELSE_NETWORK = 1;
    public static final int LOAD_NO_CACHE = 2;
    public static final int LOAD_CACHE_ONLY = 3;
    public abstract void setOffscreenPreRaster(boolean enabled);
    public abstract boolean getOffscreenPreRaster();
    public abstract void setSafeBrowsingEnabled(boolean enabled);
    public abstract boolean getSafeBrowsingEnabled();
    public abstract void setAlgorithmicDarkeningAllowed(boolean allow);
    public abstract boolean isAlgorithmicDarkeningAllowed();
    public abstract void setDisabledActionModeMenuItems(int menuItems);
    public abstract int getDisabledActionModeMenuItems();
}
""",
    "android/webkit/ServiceWorkerWebSettings.java": """package android.webkit;
public abstract class ServiceWorkerWebSettings {
    public abstract void setCacheMode(int mode);
    public abstract int getCacheMode();
    public abstract void setAllowContentAccess(boolean allow);
    public abstract boolean getAllowContentAccess();
    public abstract void setAllowFileAccess(boolean allow);
    public abstract boolean getAllowFileAccess();
    public abstract void setBlockNetworkLoads(boolean flag);
    public abstract boolean getBlockNetworkLoads();
}
""",
    "android/webkit/ServiceWorkerClient.java": """package android.webkit;
public class ServiceWorkerClient {
    public WebResourceResponse shouldInterceptRequest(WebResourceRequest request) {
        return null;
    }
}
""",
    "android/webkit/ServiceWorkerController.java": """package android.webkit;
public abstract class ServiceWorkerController {
    public abstract ServiceWorkerWebSettings getServiceWorkerWebSettings();
    public abstract void setServiceWorkerClient(ServiceWorkerClient client);
}
""",
    "android/webkit/WebStorage.java": """package android.webkit;
import java.util.Map;
public class WebStorage {
    public void getOrigins(ValueCallback<Map> callback) {}
    public void getUsageForOrigin(String origin, ValueCallback<Long> callback) {}
    public void getQuotaForOrigin(String origin, ValueCallback<Long> callback) {}
    public void setQuotaForOrigin(String origin, long quota) {}
    public void deleteOrigin(String origin) {}
    public void deleteAllData() {}
}
""",
    "android/webkit/WebMessage.java": """package android.webkit;
public class WebMessage {
    private final String mData;
    private final WebMessagePort[] mPorts;
    public WebMessage(String data) { this(data, null); }
    public WebMessage(String data, WebMessagePort[] ports) { mData = data; mPorts = ports; }
    public String getData() { return mData; }
    public WebMessagePort[] getPorts() { return mPorts; }
}
""",
    "android/webkit/WebMessagePort.java": """package android.webkit;
import android.os.Handler;
public abstract class WebMessagePort {
    public static abstract class WebMessageCallback {
        public void onMessage(WebMessagePort port, WebMessage message) {}
    }
    public abstract void postMessage(WebMessage message);
    public abstract void close();
    public abstract void setWebMessageCallback(WebMessageCallback callback);
    public abstract void setWebMessageCallback(WebMessageCallback callback, Handler handler);
}
""",
    "android/webkit/WebViewClient.java": """package android.webkit;
public class WebViewClient {}
""",
    "android/webkit/WebChromeClient.java": """package android.webkit;
public class WebChromeClient {}
""",
    "android/webkit/WebViewProvider.java": """package android.webkit;
public interface WebViewProvider {}
""",
    "android/webkit/WebView.java": """package android.webkit;
import android.content.Context;
public class WebView {
    public static class PrivateAccess {}
    public static abstract class VisualStateCallback {
        public abstract void onComplete(long requestId);
    }
    private final Context mContext;
    public WebView(Context context) { mContext = context; }
    public Context getContext() { return mContext; }
    public WebViewProvider getWebViewProvider() { return null; }
}
""",
    "android/webkit/CookieManager.java": """package android.webkit;
public abstract class CookieManager {}
""",
    "android/webkit/GeolocationPermissions.java": """package android.webkit;
public class GeolocationPermissions {}
""",
    "android/webkit/TokenBindingService.java": """package android.webkit;
public abstract class TokenBindingService {}
""",
    "android/webkit/TracingConfig.java": """package android.webkit;
public class TracingConfig {}
""",
    "android/webkit/TracingController.java": """package android.webkit;
import java.io.OutputStream;
import java.util.concurrent.Executor;
public abstract class TracingController {
    public abstract void start(TracingConfig tracingConfig);
    public abstract boolean stop(OutputStream outputStream, Executor executor);
    public abstract boolean isTracing();
}
""",
    "android/webkit/WebIconDatabase.java": """package android.webkit;
public abstract class WebIconDatabase {}
""",
    "android/webkit/WebViewDatabase.java": """package android.webkit;
public abstract class WebViewDatabase {
    public abstract boolean hasUsernamePassword();
    public abstract void clearUsernamePassword();
    public abstract boolean hasHttpAuthUsernamePassword();
    public abstract void clearHttpAuthUsernamePassword();
    public abstract void setHttpAuthUsernamePassword(String host, String realm, String u, String p);
    public abstract String[] getHttpAuthUsernamePassword(String host, String realm);
    public abstract boolean hasFormData();
    public abstract void clearFormData();
}
""",
    "android/webkit/WebViewFactoryProvider.java": """package android.webkit;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import java.util.List;
public interface WebViewFactoryProvider {
    interface Statics {
        String findAddress(String addr);
        String getDefaultUserAgent(Context context);
        void freeMemoryForTests();
        void setWebContentsDebuggingEnabled(boolean enable);
        void clearClientCertPreferences(Runnable onCleared);
        void enableSlowWholeDocumentDraw();
        Uri[] parseFileChooserResult(int resultCode, Intent intent);
        void initSafeBrowsing(Context context, ValueCallback<Boolean> callback);
        void setSafeBrowsingWhitelist(List<String> hosts, ValueCallback<Boolean> callback);
        Uri getSafeBrowsingPrivacyPolicyUrl();
    }
    Statics getStatics();
    WebViewProvider createWebView(WebView webView, WebView.PrivateAccess privateAccess);
    GeolocationPermissions getGeolocationPermissions();
    CookieManager getCookieManager();
    TokenBindingService getTokenBindingService();
    TracingController getTracingController();
    ServiceWorkerController getServiceWorkerController();
    WebIconDatabase getWebIconDatabase();
    WebStorage getWebStorage();
    WebViewDatabase getWebViewDatabase(Context context);
    ClassLoader getWebViewClassLoader();
}
""",
    "org/json/JSONException.java": """package org.json;
public class JSONException extends Exception {
    public JSONException(String s) { super(s); }
}
""",
    "org/json/JSONObject.java": """package org.json;
import java.util.LinkedHashMap;
import java.util.Map;
public class JSONObject {
    private final Map<String, Object> mMap = new LinkedHashMap<>();
    public JSONObject put(String key, Object value) throws JSONException {
        mMap.put(key, value);
        return this;
    }
    public boolean optBoolean(String key) {
        Object v = mMap.get(key);
        return v instanceof Boolean && (Boolean) v;
    }
    public String optString(String key) {
        return optString(key, "");
    }
    public String optString(String key, String fallback) {
        Object v = mMap.get(key);
        return v instanceof String ? (String) v : fallback;
    }
    public String getString(String key) throws JSONException {
        Object v = mMap.get(key);
        if (!(v instanceof String)) throw new JSONException("Missing " + key);
        return (String) v;
    }
}
""",
    # Package-private sibling adapters inside com.gesher.app.webview
    "com/gesher/app/webview/GesherWebView.java": """package com.gesher.app.webview;
public final class GesherWebView {
    static final String TAG = "gesher-webview";
    static void log(String message) {}
    static String defaultUserAgent() { return "GesherTestUA/1.0"; }
}
""",
    "com/gesher/app/webview/GesherCookieManager.java": """package com.gesher.app.webview;
import android.webkit.CookieManager;
final class GesherCookieManager extends CookieManager {}
""",
    "com/gesher/app/webview/GesherGeolocationPermissions.java": """package com.gesher.app.webview;
import android.webkit.GeolocationPermissions;
final class GesherGeolocationPermissions extends GeolocationPermissions {}
""",
    "com/gesher/app/webview/WebView2Host.java": """package com.gesher.app.webview;
import java.util.ArrayList;
import java.util.List;
public final class WebView2Host {
    public static final int DATA_FILE_SYSTEMS = 0x1;
    public static final int DATA_INDEXED_DB = 0x2;
    public static final int DATA_LOCAL_STORAGE = 0x4;
    public static final int DATA_WEB_SQL = 0x8;
    public static final int DATA_CACHE_STORAGE = 0x10;
    public static final int DATA_ALL_DOM_STORAGE = 0x20;
    static final List<Integer> browsingClears = new ArrayList<>();
    static final List<String> originClears = new ArrayList<>();
    public static boolean setProxyArguments(String args) { return true; }
    public static boolean clearBrowsingData(int kinds, Object callback) {
        browsingClears.add(kinds);
        return true;
    }
    public static boolean clearOriginData(String origin, Object callback) {
        originClears.add(origin);
        return true;
    }
}
""",
    "com/gesher/app/webview/GesherWebViewProvider.java": """package com.gesher.app.webview;
import android.net.Uri;
import android.webkit.WebChromeClient;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.WebViewProvider;
import java.lang.reflect.InvocationHandler;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
final class GesherWebViewProvider implements WebViewProvider {
    static volatile boolean sDevTools;
    static final class StartScript {}
    static final class MessageListener {
        final String name;
        final InvocationHandler listener;
        final Map<String, Object> replyProxies = new ConcurrentHashMap<>();
        MessageListener(String name, InvocationHandler listener) {
            this.name = name; this.listener = listener;
        }
    }
    GesherWebViewProvider(WebView view, WebView.PrivateAccess access) {}
    WebView view() { return null; }
    String currentUrl() { return "https://example.test/"; }
    static String originOf(String url) { return "https://example.test/"; }
    void insertVisualStateCallback(long id, WebView.VisualStateCallback cb) {}
    WebMessagePort[] createWebMessageChannel() { return new WebMessagePort[0]; }
    void postMessageToMainFrame(WebMessage message, Uri origin) {}
    void addMessageListener(String name, String[] rules, InvocationHandler listener) {}
    void removeMessageListener(String name) {}
    StartScript addStartScript(String script, String[] rules) { return new StartScript(); }
    void removeStartScript(StartScript s) {}
    WebViewClient getWebViewClient() { return null; }
    WebChromeClient getWebChromeClient() { return null; }
    void postToListener(String name, Object message) {}
}
""",
}

TEST_HARNESS = r"""package com.gesher.app.webview;

import android.content.ContentResolver;
import android.content.Context;
import android.content.res.AssetManager;
import android.net.Uri;
import android.webkit.ServiceWorkerClient;
import android.webkit.ServiceWorkerController;
import android.webkit.ServiceWorkerWebSettings;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebStorage;

import org.chromium.support_lib_boundary.ServiceWorkerClientBoundaryInterface;
import org.chromium.support_lib_boundary.ServiceWorkerControllerBoundaryInterface;
import org.chromium.support_lib_boundary.ServiceWorkerWebSettingsBoundaryInterface;
import org.chromium.support_lib_boundary.WebViewProviderFactoryBoundaryInterface;
import org.chromium.support_lib_boundary.WebkitToCompatConverterBoundaryInterface;

import java.io.ByteArrayInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.net.CacheRequest;
import java.net.CacheResponse;
import java.net.ResponseCache;
import java.net.URI;
import java.net.URLConnection;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class ServiceWorkerAndStorageContractTest {
    private static int checks;

    private static void check(boolean cond, String message) {
        if (!cond) throw new AssertionError(message);
        checks++;
    }

    private static <T> T castBoundary(Class<T> iface, InvocationHandler handler) {
        check(handler != null, "boundary handler for " + iface.getSimpleName() + " was null");
        return iface.cast(Proxy.newProxyInstance(iface.getClassLoader(), new Class<?>[] {iface},
                handler));
    }

    private static String readUtf8(WebResourceResponse response) throws IOException {
        if (response == null || response.getData() == null) return "";
        try (InputStream in = response.getData()) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        }
    }

    private static void verifyFeaturesAndSingletonWiring() {
        InvocationHandler rawFactory = SupportLib.createWebViewProviderFactory();
        WebViewProviderFactoryBoundaryInterface factory =
                castBoundary(WebViewProviderFactoryBoundaryInterface.class, rawFactory);

        Set<String> features = new HashSet<>(Arrays.asList(factory.getSupportedFeatures()));
        String[] required = {
            "SERVICE_WORKER_BASIC_USAGE",
            "SERVICE_WORKER_CACHE_MODE",
            "SERVICE_WORKER_CONTENT_ACCESS",
            "SERVICE_WORKER_FILE_ACCESS",
            "SERVICE_WORKER_BLOCK_NETWORK_LOADS",
            "SERVICE_WORKER_SHOULD_INTERCEPT_REQUEST",
        };
        for (String flag : required) {
            check(features.contains(flag), "missing advertised feature " + flag);
        }
        // Must NOT invent unimplemented capabilities
        check(!features.contains("DELETE_BROWSING_DATA"),
                "must not advertise unimplemented DELETE_BROWSING_DATA");
        check(!features.contains("TRACING_CONTROLLER_BASIC_USAGE"),
                "must not advertise unimplemented TRACING_CONTROLLER_BASIC_USAGE");

        ServiceWorkerControllerBoundaryInterface boundarySw = castBoundary(
                ServiceWorkerControllerBoundaryInterface.class,
                factory.getServiceWorkerController());
        ServiceWorkerWebSettingsBoundaryInterface boundarySettings = castBoundary(
                ServiceWorkerWebSettingsBoundaryInterface.class,
                boundarySw.getServiceWorkerWebSettings());

        GesherWebViewFactory providerFactory = new GesherWebViewFactory();
        ServiceWorkerController fwSw = providerFactory.getServiceWorkerController();
        ServiceWorkerWebSettings fwSettings = fwSw.getServiceWorkerWebSettings();
        check(fwSw == new GesherWebViewFactory().getServiceWorkerController(),
                "ServiceWorkerController must be a process singleton across factories");

        // Verify defaults match Android / Chromium ServiceWorkerWebSettings contract
        check(fwSettings.getCacheMode() == WebSettings.LOAD_DEFAULT
                && boundarySettings.getCacheMode() == WebSettings.LOAD_DEFAULT,
                "default cacheMode must be WebSettings.LOAD_DEFAULT (-1)");
        check(fwSettings.getAllowContentAccess() && boundarySettings.getAllowContentAccess(),
                "default allowContentAccess must be true");
        check(!fwSettings.getAllowFileAccess() && !boundarySettings.getAllowFileAccess(),
                "default allowFileAccess must be false on modern API levels");
        check(!fwSettings.getBlockNetworkLoads() && !boundarySettings.getBlockNetworkLoads(),
                "default blockNetworkLoads must be false");

        // Verify bidirectional settings synchronization and WebkitToCompatConverter round-trip
        boundarySettings.setCacheMode(WebSettings.LOAD_NO_CACHE);
        check(fwSettings.getCacheMode() == WebSettings.LOAD_NO_CACHE,
                "boundary setCacheMode did not update framework settings");
        fwSettings.setCacheMode(WebSettings.LOAD_DEFAULT);
        check(boundarySettings.getCacheMode() == WebSettings.LOAD_DEFAULT,
                "framework setCacheMode did not update boundary settings");

        WebkitToCompatConverterBoundaryInterface converter = castBoundary(
                WebkitToCompatConverterBoundaryInterface.class,
                factory.getWebkitToCompatConverter());
        InvocationHandler convertedHandler = converter.convertServiceWorkerSettings(fwSettings);
        ServiceWorkerWebSettingsBoundaryInterface convertedProxy = castBoundary(
                ServiceWorkerWebSettingsBoundaryInterface.class, convertedHandler);
        convertedProxy.setAllowFileAccess(true);
        check(fwSettings.getAllowFileAccess(), "converted settings handler did not mutate original");
        fwSettings.setAllowFileAccess(false);
        check(converter.convertServiceWorkerSettings(convertedHandler) == fwSettings,
                "convertServiceWorkerSettings(InvocationHandler) did not unwrap original settings");

        try {
            converter.convertServiceWorkerSettings("not-settings");
            throw new AssertionError("accepted non-ServiceWorkerWebSettings object");
        } catch (IllegalArgumentException expected) { checks++; }
        try {
            converter.convertServiceWorkerSettings((proxy, method, args) -> null);
            throw new AssertionError("accepted foreign InvocationHandler");
        } catch (IllegalArgumentException expected) { checks++; }
    }

    private static void verifySettingsAndClientEnforcement() throws Exception {
        GesherWebViewFactory.GesherServiceWorkers sw = GesherWebViewFactory.serviceWorkers();
        ServiceWorkerWebSettings settings = sw.getServiceWorkerWebSettings();
        // Reset state cleanly
        sw.setServiceWorkerClient(null);
        settings.setCacheMode(WebSettings.LOAD_DEFAULT);
        settings.setAllowContentAccess(true);
        settings.setAllowFileAccess(false);
        settings.setBlockNetworkLoads(false);

        AtomicInteger contentOpens = new AtomicInteger();
        Context fakeContext = new Context() {
            @Override
            public ContentResolver getContentResolver() {
                return new ContentResolver() {
                    @Override
                    public InputStream openInputStream(Uri uri) throws IOException {
                        contentOpens.incrementAndGet();
                        if ("content://com.test.provider/sw.js".equals(uri.toString())) {
                            return new ByteArrayInputStream(
                                    "self.ok=1;".getBytes(StandardCharsets.UTF_8));
                        }
                        throw new FileNotFoundException(uri.toString());
                    }
                    @Override
                    public String getType(Uri uri) {
                        return "application/javascript";
                    }
                };
            }
            @Override
            public AssetManager getAssets() {
                return new AssetManager() {
                    @Override
                    public InputStream open(String fileName) throws IOException {
                        if ("worker.js".equals(fileName)) {
                            return new ByteArrayInputStream(
                                    "/*asset-worker*/".getBytes(StandardCharsets.UTF_8));
                        }
                        throw new FileNotFoundException(fileName);
                    }
                };
            }
        };
        sw.attachContext(fakeContext);

        // 1. allowContentAccess: true -> 200 OK; false -> 403 Blocked without opening ContentResolver
        WebResourceResponse contentOk = sw.intercept(null, "content://com.test.provider/sw.js",
                "GET", Map.of());
        check(contentOk != null && contentOk.getStatusCode() == 200
                && "application/javascript".equals(contentOk.getMimeType())
                && "self.ok=1;".equals(readUtf8(contentOk)) && contentOpens.get() == 1,
                "content:// access failed when allowContentAccess=true");

        settings.setAllowContentAccess(false);
        WebResourceResponse contentBlocked = sw.intercept(null,
                "content://com.test.provider/sw.js", "GET", Map.of());
        check(contentBlocked != null && contentBlocked.getStatusCode() == 403
                && contentOpens.get() == 1,
                "content:// was not blocked when allowContentAccess=false");
        settings.setAllowContentAccess(true);

        // 2. allowFileAccess: disk file blocked (404) when false, served (200) when true;
        //    file:///android_asset/ always served even when allowFileAccess=false
        Path tempFile = Files.createTempFile("gesher-sw-", ".js");
        try {
            Files.writeString(tempFile, "console.log('disk-sw');", StandardCharsets.UTF_8);
            String fileUrl = "file://" + tempFile.toAbsolutePath();
            String mappedUrl = FileUrls.toWebView2(fileUrl);

            settings.setAllowFileAccess(false);
            WebResourceResponse deniedDisk = sw.intercept(null, mappedUrl, "GET", Map.of());
            check(deniedDisk != null && deniedDisk.getStatusCode() == 404,
                    "disk file was served while allowFileAccess=false");

            WebResourceResponse assetResp = sw.intercept(null,
                    FileUrls.toWebView2("file:///android_asset/worker.js"), "GET", Map.of());
            check(assetResp != null && assetResp.getStatusCode() == 200
                    && "/*asset-worker*/".equals(readUtf8(assetResp)),
                    "android_asset failed when allowFileAccess=false");

            settings.setAllowFileAccess(true);
            WebResourceResponse allowedDisk = sw.intercept(null, mappedUrl, "GET", Map.of());
            check(allowedDisk != null && allowedDisk.getStatusCode() == 200
                    && "text/javascript".equals(allowedDisk.getMimeType())
                    && "console.log('disk-sw');".equals(readUtf8(allowedDisk)),
                    "disk file was not served when allowFileAccess=true");
            settings.setAllowFileAccess(false);
        } finally {
            Files.deleteIfExists(tempFile);
        }

        // 3. blockNetworkLoads: false -> null (network pass-through); true -> 403 Blocked
        check(sw.intercept(null, "https://app.example/sw.js", "GET", Map.of()) == null,
                "default network load should pass through");
        settings.setBlockNetworkLoads(true);
        WebResourceResponse netBlocked = sw.intercept(null, "https://app.example/sw.js", "GET",
                Map.of());
        check(netBlocked != null && netBlocked.getStatusCode() == 403,
                "network load was not blocked when blockNetworkLoads=true");
        settings.setBlockNetworkLoads(false);

        // 4. cacheMode enforcement (LOAD_CACHE_ONLY, LOAD_CACHE_ELSE_NETWORK, LOAD_NO_CACHE, LOAD_DEFAULT)
        ResponseCache previousCache = ResponseCache.getDefault();
        try {
            ResponseCache.setDefault(new ResponseCache() {
                @Override
                public CacheResponse get(URI uri, String rqstMethod,
                        Map<String, List<String>> rqstHeaders) {
                    if ("https://cached.example/hit.js".equals(uri.toString())) {
                        return new CacheResponse() {
                            @Override
                            public Map<String, List<String>> getHeaders() {
                                Map<String, List<String>> h = new LinkedHashMap<>();
                                h.put("Content-Type",
                                        Collections.singletonList("text/javascript; charset=UTF-8"));
                                return h;
                            }
                            @Override
                            public InputStream getBody() {
                                return new ByteArrayInputStream(
                                        "cached();".getBytes(StandardCharsets.UTF_8));
                            }
                        };
                    }
                    return null;
                }
                @Override
                public CacheRequest put(URI uri, URLConnection conn) { return null; }
            });

            settings.setCacheMode(WebSettings.LOAD_CACHE_ONLY);
            WebResourceResponse cacheHit = sw.intercept(null, "https://cached.example/hit.js",
                    "GET", Map.of());
            check(cacheHit != null && cacheHit.getStatusCode() == 200
                    && "text/javascript".equals(cacheHit.getMimeType())
                    && "UTF-8".equals(cacheHit.getEncoding())
                    && "cached();".equals(readUtf8(cacheHit)),
                    "LOAD_CACHE_ONLY failed to return cached response");
            WebResourceResponse cacheMiss = sw.intercept(null, "https://cached.example/miss.js",
                    "GET", Map.of());
            check(cacheMiss != null && cacheMiss.getStatusCode() == 504,
                    "LOAD_CACHE_ONLY allowed network load on cache miss");
            check("only-if-cached".equals(sw.effectiveRequestHeaders(Map.of()).get("Cache-Control")),
                    "LOAD_CACHE_ONLY missing only-if-cached header");

            settings.setCacheMode(WebSettings.LOAD_CACHE_ELSE_NETWORK);
            WebResourceResponse cenHit = sw.intercept(null, "https://cached.example/hit.js",
                    "GET", Map.of());
            check(cenHit != null && "cached();".equals(readUtf8(cenHit)),
                    "LOAD_CACHE_ELSE_NETWORK missed cached entry");
            check(sw.intercept(null, "https://cached.example/miss.js", "GET", Map.of()) == null,
                    "LOAD_CACHE_ELSE_NETWORK did not fall back to network on miss");
            check("max-stale=2147483647".equals(
                    sw.effectiveRequestHeaders(Map.of()).get("Cache-Control")),
                    "LOAD_CACHE_ELSE_NETWORK wrong Cache-Control header");

            settings.setCacheMode(WebSettings.LOAD_NO_CACHE);
            check(sw.intercept(null, "https://cached.example/hit.js", "GET", Map.of()) == null,
                    "LOAD_NO_CACHE did not bypass cache");
            Map<String, String> noCacheHeaders = sw.effectiveRequestHeaders(Map.of("Accept", "*/*"));
            check("no-cache".equals(noCacheHeaders.get("Cache-Control"))
                    && "no-cache".equals(noCacheHeaders.get("Pragma"))
                    && "*/*".equals(noCacheHeaders.get("Accept")),
                    "LOAD_NO_CACHE headers incorrect");

            settings.setCacheMode(WebSettings.LOAD_DEFAULT);
            check(sw.effectiveRequestHeaders(Map.of("Accept", "*/*")).size() == 1,
                    "LOAD_DEFAULT unexpectedly mutated headers");
        } finally {
            ResponseCache.setDefault(previousCache);
        }

        // 5. ServiceWorkerClient.shouldInterceptRequest via SupportLib boundary AND framework
        WebViewProviderFactoryBoundaryInterface factory = castBoundary(
                WebViewProviderFactoryBoundaryInterface.class,
                SupportLib.createWebViewProviderFactory());
        ServiceWorkerControllerBoundaryInterface boundarySw = castBoundary(
                ServiceWorkerControllerBoundaryInterface.class,
                factory.getServiceWorkerController());

        // 5a. Boundary client WITHOUT SERVICE_WORKER_SHOULD_INTERCEPT_REQUEST feature is ignored
        AtomicInteger unsupportedCalls = new AtomicInteger();
        InvocationHandler unsupportedClient = (proxy, method, args) -> {
            if ("getSupportedFeatures".equals(method.getName())) return new String[0];
            if ("shouldInterceptRequest".equals(method.getName())) {
                unsupportedCalls.incrementAndGet();
                return new WebResourceResponse("text/plain", "UTF-8",
                        new ByteArrayInputStream("bad".getBytes(StandardCharsets.UTF_8)));
            }
            return null;
        };
        boundarySw.setServiceWorkerClient(unsupportedClient);
        check(sw.intercept(null, "https://app.example/sw.js", "GET", Map.of()) == null
                && unsupportedCalls.get() == 0,
                "invoked boundary ServiceWorkerClient that lacked feature flag");

        // 5b. Boundary client WITH SERVICE_WORKER_SHOULD_INTERCEPT_REQUEST intercepts even when blockNetworkLoads=true
        AtomicReference<WebResourceRequest> seenReq = new AtomicReference<>();
        InvocationHandler activeClient = (proxy, method, args) -> {
            if ("getSupportedFeatures".equals(method.getName())) {
                return new String[] {"SERVICE_WORKER_SHOULD_INTERCEPT_REQUEST"};
            }
            if ("shouldInterceptRequest".equals(method.getName())) {
                WebResourceRequest req = (WebResourceRequest) args[0];
                seenReq.set(req);
                return new WebResourceResponse("text/javascript", "UTF-8", 200, "OK",
                        Map.of("X-Intercepted", "boundary"),
                        new ByteArrayInputStream("intercepted-body".getBytes(StandardCharsets.UTF_8)));
            }
            return null;
        };
        settings.setBlockNetworkLoads(true);
        boundarySw.setServiceWorkerClient(activeClient);
        WebResourceResponse custom = sw.intercept(null,
                FileUrls.toWebView2("file:///android_asset/custom.js"), "POST",
                Map.of("X-Req", "1"));
        check(custom != null && "intercepted-body".equals(readUtf8(custom))
                && "boundary".equals(custom.getResponseHeaders().get("X-Intercepted")),
                "boundary ServiceWorkerClient response was not returned");
        check(seenReq.get() != null
                && !seenReq.get().isForMainFrame()
                && "file:///android_asset/custom.js".equals(seenReq.get().getUrl().toString())
                && "POST".equals(seenReq.get().getMethod())
                && "1".equals(seenReq.get().getRequestHeaders().get("X-Req")),
                "ServiceWorkerClient received wrong WebResourceRequest metadata");

        // 5c. Clearing boundary client with null restores normal blockNetworkLoads enforcement
        boundarySw.setServiceWorkerClient(null);
        WebResourceResponse afterClear = sw.intercept(null, "https://app.example/sw.js", "GET",
                Map.of());
        check(afterClear != null && afterClear.getStatusCode() == 403,
                "setServiceWorkerClient(null) did not clear client");
        settings.setBlockNetworkLoads(false);

        // 5d. Direct framework ServiceWorkerClient also works on the shared singleton
        sw.setServiceWorkerClient(new ServiceWorkerClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebResourceRequest request) {
                return new WebResourceResponse("text/plain", "UTF-8",
                        new ByteArrayInputStream("fw-client".getBytes(StandardCharsets.UTF_8)));
            }
        });
        check("fw-client".equals(readUtf8(sw.shouldInterceptRequest(seenReq.get()))),
                "framework ServiceWorkerClient did not intercept");
        sw.setServiceWorkerClient(null);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private static void verifyWebStorageNoSyntheticQuotaContracts() {
        WebView2Host.browsingClears.clear();
        WebView2Host.originClears.clear();
        WebStorage storage = new GesherWebViewFactory().getWebStorage();

        AtomicReference<Map> originsRef = new AtomicReference<>();
        storage.getOrigins(originsRef::set);
        Map origins = originsRef.get();
        check(origins != null && origins.isEmpty(), "getOrigins invented synthetic origins");
        // Callers may mutate the returned Map without UnsupportedOperationException
        origins.put("https://scratch.invalid", "test");
        AtomicReference<Map> secondRef = new AtomicReference<>();
        storage.getOrigins(secondRef::set);
        check(secondRef.get() != null && secondRef.get().isEmpty(),
                "mutating returned origins map polluted subsequent getOrigins calls");

        AtomicReference<Long> usage = new AtomicReference<>(-1L);
        AtomicReference<Long> quota = new AtomicReference<>(-1L);
        storage.getUsageForOrigin("https://example.test", usage::set);
        storage.getQuotaForOrigin("https://example.test", quota::set);
        check(Long.valueOf(0L).equals(usage.get()) && Long.valueOf(0L).equals(quota.get()),
                "invented non-zero usage or quota values");
        storage.setQuotaForOrigin("https://example.test", 1048576L);
        storage.getQuotaForOrigin("https://example.test", quota::set);
        check(Long.valueOf(0L).equals(quota.get()),
                "deprecated setQuotaForOrigin invented synthetic quota");

        storage.getOrigins(null);
        storage.getUsageForOrigin(null, null);
        storage.getQuotaForOrigin(null, null);
        storage.deleteOrigin(null);
        storage.deleteOrigin("");
        check(WebView2Host.originClears.isEmpty() && WebView2Host.browsingClears.isEmpty(),
                "null/empty origin triggered native clearing");

        storage.deleteOrigin("https://origin-one.test:8443");
        check(WebView2Host.originClears.equals(List.of("https://origin-one.test:8443"))
                && WebView2Host.browsingClears.isEmpty(),
                "deleteOrigin did not forward exact origin to clearOriginData");

        storage.deleteAllData();
        check(WebView2Host.browsingClears.equals(List.of(GesherWebStorage.DOM_DATA))
                && WebView2Host.originClears.size() == 1,
                "deleteAllData did not forward DOM_DATA mask to clearBrowsingData");
    }

    public static void main(String[] args) throws Exception {
        verifyFeaturesAndSingletonWiring();
        verifySettingsAndClientEnforcement();
        verifyWebStorageNoSyntheticQuotaContracts();
        System.out.println("ServiceWorkerAndStorageContractTest: " + checks + " checks passed");
    }
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", required=True, type=Path)
    args = parser.parse_args()

    javac = str(args.java_home / "bin/javac")
    java = str(args.java_home / "bin/java")

    with tempfile.TemporaryDirectory(prefix="gesher-sw-contracts-") as tmp:
        work = Path(tmp)
        cls_dir = work / "classes"
        cls_dir.mkdir()
        inputs: list[str] = []

        for rel, content in STUBS.items():
            path = work / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            inputs.append(str(path))

        prod_files = [
            "SupportLib.java",
            "GesherWebViewFactory.java",
            "GesherWebStorage.java",
            "FileUrls.java",
            "WebMessagePayload.java",
        ]
        for name in prod_files:
            src = (ROOT / "app/java/com/gesher/app/webview" / name).read_text(encoding="utf-8")
            dst = work / "com/gesher/app/webview" / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(src, encoding="utf-8")
            inputs.append(str(dst))

        harness = work / "com/gesher/app/webview/ServiceWorkerAndStorageContractTest.java"
        harness.write_text(TEST_HARNESS, encoding="utf-8")
        inputs.append(str(harness))

        subprocess.run(
            [javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-d", str(cls_dir), *inputs],
            check=True, timeout=60
        )
        subprocess.run(
            [java, "-XX:-UsePerfData", "-cp", str(cls_dir),
             "com.gesher.app.webview.ServiceWorkerAndStorageContractTest"],
            check=True, timeout=60
        )


if __name__ == "__main__":
    main()
