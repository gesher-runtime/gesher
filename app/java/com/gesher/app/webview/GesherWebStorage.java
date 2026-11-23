package com.gesher.app.webview;

import android.webkit.ValueCallback;
import android.webkit.WebStorage;

import java.util.Collections;
import java.util.Map;

/**
 * WebStorage over WebView2's profile (docs/webview.md). HTTP(S) origin deletion
 * removes its first-party file systems and IndexedDB. Unsupported origins do
 * nothing. Per-origin usage/quota comes from Chromium. Full origin enumeration
 * and partitioned third-party deletion remain unsupported.
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
        WebStorageQueries.query(origin, false, callback);
    }

    @Override
    public void getQuotaForOrigin(String origin, ValueCallback<Long> callback) {
        WebStorageQueries.query(origin, true, callback);
    }

    @Override
    public void deleteOrigin(String origin) {
        if (origin == null || origin.isEmpty()) return;
        WebView2Host.clearOriginData(origin, null);
    }

    @Override
    public void deleteAllData() {
        WebView2Host.clearBrowsingData(DOM_DATA, null);
    }
}
