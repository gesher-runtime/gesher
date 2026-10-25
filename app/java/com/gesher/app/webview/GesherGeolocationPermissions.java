package com.gesher.app.webview;

import android.webkit.GeolocationPermissions;
import android.webkit.ValueCallback;

import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * GeolocationPermissions (docs/webview.md, "Permissions"): the origins whose decision the
 * app retained (Callback.invoke(origin, allow, true)), for this process's lifetime.
 * WebView2 keeps the decision too while it is retained (the profile saves it).
 */
final class GesherGeolocationPermissions extends GeolocationPermissions {
    private final Map<String, Boolean> mRetained = new ConcurrentHashMap<>();

    /** The retained decision for the origin, or null. */
    Boolean retained(String origin) {
        return origin != null ? mRetained.get(origin) : null;
    }

    void retain(String origin, boolean allow) {
        if (origin != null) mRetained.put(origin, allow);
    }

    @Override
    public void getOrigins(ValueCallback<Set<String>> callback) {
        if (callback != null) callback.onReceiveValue(new HashSet<>(mRetained.keySet()));
    }

    @Override
    public void getAllowed(String origin, ValueCallback<Boolean> callback) {
        if (callback != null) callback.onReceiveValue(Boolean.TRUE.equals(retained(origin)));
    }

    @Override
    public void clear(String origin) {
        if (origin != null) mRetained.remove(origin);
    }

    @Override
    public void allow(String origin) {
        retain(origin, true);
    }

    @Override
    public void clearAll() {
        mRetained.clear();
    }
}
