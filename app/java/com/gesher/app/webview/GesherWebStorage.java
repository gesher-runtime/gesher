package com.gesher.app.webview;

import android.webkit.ValueCallback;
import android.webkit.WebStorage;

import java.util.Collections;
import java.util.Map;

/**
 * WebStorage over WebView2's profile (docs/webview.md): deleting data clears the
 * profile's DOM storage (local storage, IndexedDB, Web SQL, file systems, cache storage).
 * Quotas and per-origin usage are not reported.
 */
final class GesherWebStorage extends WebStorage {
    static final int DOM_DATA = WebView2Host.DATA_ALL_DOM_STORAGE | WebView2Host.DATA_FILE_SYSTEMS
            | WebView2Host.DATA_INDEXED_DB | WebView2Host.DATA_LOCAL_STORAGE
            | WebView2Host.DATA_WEB_SQL | WebView2Host.DATA_CACHE_STORAGE;

    @Override
    @SuppressWarnings({"rawtypes", "unchecked"})
    public void getOrigins(ValueCallback<Map> callback) {
        if (callback != null) callback.onReceiveValue(Collections.emptyMap());
    }

    @Override
    public void getUsageForOrigin(String origin, ValueCallback<Long> callback) {
        if (callback != null) callback.onReceiveValue(0L);
    }

    @Override
    public void getQuotaForOrigin(String origin, ValueCallback<Long> callback) {
        if (callback != null) callback.onReceiveValue(0L);
    }

    @Override
    public void deleteOrigin(String origin) {
        deleteAllData();
    }

    @Override
    public void deleteAllData() {
        WebView2Host.clearBrowsingData(DOM_DATA, null);
    }
}
