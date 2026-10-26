package com.gesher.app.webview;

import android.content.pm.PackageManager;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.CancellationSignal;
import android.os.Looper;
import android.webkit.GeolocationPermissions;
import android.webkit.WebChromeClient;

import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * navigator.geolocation answered from Gesher's LocationManager (ADR 0058), as Android's
 * WebView answers it from the device's: the app's WebChromeClient decides per origin
 * (onGeolocationPermissionsShowPrompt, GeolocationPermissions' retained answers), the app
 * needs a location permission, and the positions are LocationManager's (Windows' location
 * service, or the test providers an app adds). WebView2's own geolocation (Windows'
 * location service through Chromium, which the OS may deny) is not used: a document script
 * ({@link #SCRIPT}) replaces getCurrentPosition, watchPosition and clearWatch in main frames
 * and asks over chrome.webview messages.
 */
final class GeolocationShim {
    /**
     * The page side: requests go out as chrome.webview.postMessage({gesherGeo: "get" |
     * "watch" | "clear", id, highAccuracy, timeout, maximumAge}); answers come as message
     * events {gesher: "geo", id, coords..., timestamp} or {gesher: "geo", id, error, message}.
     */
    static final String SCRIPT = "(function(){\n"
            + "if (window.top !== window || !window.chrome || !chrome.webview"
            + " || !window.Geolocation || window.__gesherGeo) return;\n"
            + "Object.defineProperty(window, '__gesherGeo', {value: true});\n"
            + "var next = 1, pending = {};\n"
            + "function error(code, message) {\n"
            + "  var e = Object.create(window.GeolocationPositionError ?"
            + " GeolocationPositionError.prototype : Object.prototype);\n"
            + "  Object.defineProperty(e, 'code', {value: code, enumerable: true});\n"
            + "  Object.defineProperty(e, 'message', {value: message || '', enumerable: true});\n"
            + "  return e;\n"
            + "}\n"
            + "function position(m) {\n"
            + "  var c = {latitude: m.latitude, longitude: m.longitude, accuracy: m.accuracy,\n"
            + "    altitude: m.altitude === undefined ? null : m.altitude,\n"
            + "    altitudeAccuracy: m.altitudeAccuracy === undefined ? null : m.altitudeAccuracy,\n"
            + "    heading: m.heading === undefined ? null : m.heading,\n"
            + "    speed: m.speed === undefined ? null : m.speed};\n"
            + "  c.toJSON = function() { var o = {}; for (var k in c) if (k !== 'toJSON') o[k] = c[k];"
            + " return o; };\n"
            + "  return {coords: c, timestamp: m.timestamp,"
            + " toJSON: function() { return {coords: c.toJSON(), timestamp: m.timestamp}; }};\n"
            + "}\n"
            + "function request(kind, ok, err, opts) {\n"
            + "  if (typeof ok !== 'function') throw new TypeError('a success callback is needed');\n"
            + "  var id = next++;\n"
            + "  pending[id] = {ok: ok, err: typeof err === 'function' ? err : null,"
            + " watch: kind === 'watch'};\n"
            + "  opts = opts || {};\n"
            + "  chrome.webview.postMessage({gesherGeo: kind, id: id,"
            + " highAccuracy: !!opts.enableHighAccuracy,"
            + " timeout: typeof opts.timeout === 'number' ? opts.timeout : -1,"
            + " maximumAge: typeof opts.maximumAge === 'number' ? opts.maximumAge : 0});\n"
            + "  return id;\n"
            + "}\n"
            + "chrome.webview.addEventListener('message', function(e) {\n"
            + "  var m = e.data;\n"
            + "  if (!m || typeof m !== 'object' || m.gesher !== 'geo') return;\n"
            + "  var p = pending[m.id];\n"
            + "  if (!p) return;\n"
            + "  if (!p.watch) delete pending[m.id];\n"
            + "  if (m.error) { if (p.err) p.err(error(m.error, m.message)); }\n"
            + "  else p.ok(position(m));\n"
            + "});\n"
            + "var proto = Geolocation.prototype;\n"
            + "Object.defineProperty(proto, 'getCurrentPosition', {configurable: true, writable: true,"
            + " value: function(ok, err, opts) { request('get', ok, err, opts); }});\n"
            + "Object.defineProperty(proto, 'watchPosition', {configurable: true, writable: true,"
            + " value: function(ok, err, opts) { return request('watch', ok, err, opts); }});\n"
            + "Object.defineProperty(proto, 'clearWatch', {configurable: true, writable: true,"
            + " value: function(id) { if (pending[id]) { delete pending[id];"
            + " chrome.webview.postMessage({gesherGeo: 'clear', id: id}); } }});\n"
            + "})();\n";

    private static final int PERMISSION_DENIED = 1;
    private static final int POSITION_UNAVAILABLE = 2;
    private static final int TIMEOUT = 3;

    private final GesherWebViewProvider mProvider;
    /** The page's answers (this document): origin -> allowed. */
    private final Map<String, Boolean> mPageAnswers = new HashMap<>();
    /** Requests waiting for the app's answer, by origin. */
    private final Map<String, List<Runnable>> mAsking = new HashMap<>();
    private final Map<Integer, LocationListener> mWatches = new HashMap<>();
    private final List<CancellationSignal> mGets = new ArrayList<>();
    /** Bumped when the page goes: answers for the old page are dropped. */
    private int mGeneration;

    GeolocationShim(GesherWebViewProvider provider) {
        mProvider = provider;
    }

    /** The page went (a navigation, destroy): its requests and watches end. */
    void pageGone() {
        mGeneration++;
        mPageAnswers.clear();
        mAsking.clear();
        LocationManager lm = locationManager();
        for (LocationListener l : mWatches.values()) {
            if (lm != null) lm.removeUpdates(l);
        }
        mWatches.clear();
        for (CancellationSignal s : mGets) s.cancel();
        mGets.clear();
    }

    /** A request from the page (the WebView's Looper). */
    void onMessage(JSONObject m, String source) {
        String kind = m.optString("gesherGeo");
        int id = m.optInt("id");
        if ("clear".equals(kind)) {
            LocationListener l = mWatches.remove(id);
            LocationManager lm = locationManager();
            if (l != null && lm != null) lm.removeUpdates(l);
            return;
        }
        boolean high = m.optBoolean("highAccuracy");
        long timeout = m.optLong("timeout", -1);
        long maximumAge = m.optLong("maximumAge", 0);
        boolean watch = "watch".equals(kind);
        String origin = GesherWebViewProvider.originOf(source != null ? source
                : mProvider.currentUrl());
        int generation = mGeneration;
        withPermission(origin, allowed -> {
            if (generation != mGeneration) return;
            if (!allowed) {
                error(id, PERMISSION_DENIED, "User denied Geolocation");
                return;
            }
            if (watch) {
                startWatch(id, high);
            } else {
                getCurrent(id, high, timeout, maximumAge, generation);
            }
        });
    }

    private interface Answer {
        void allowed(boolean allowed);
    }

    /**
     * Whether the origin may have positions: the app's location permission, the WebView's
     * settings, a retained or this page's answer, else the app's WebChromeClient (as
     * Android's WebView: without one it is denied).
     */
    private void withPermission(String origin, Answer answer) {
        android.content.Context context = mProvider.context();
        boolean appMay = hasLocationPermission(context);
        if (!appMay || !mProvider.settings().geolocationEnabled()) {
            GesherWebView.log("geolocation for " + origin + ": denied ("
                    + (!appMay ? "the app has no location permission" : "setGeolocationEnabled(false)")
                    + ")");
            answer.allowed(false);
            return;
        }
        GesherGeolocationPermissions store = (GesherGeolocationPermissions)
                GeolocationPermissions.getInstance();
        Boolean retained = store.retained(origin);
        if (retained != null) {
            answer.allowed(retained);
            return;
        }
        Boolean page = mPageAnswers.get(origin);
        if (page != null) {
            answer.allowed(page);
            return;
        }
        WebChromeClient chrome = mProvider.chrome();
        if (chrome == null) {
            answer.allowed(false);
            return;
        }
        List<Runnable> waiting = mAsking.get(origin);
        boolean ask = waiting == null;
        if (ask) {
            waiting = new ArrayList<>();
            mAsking.put(origin, waiting);
        }
        waiting.add(() -> answer.allowed(Boolean.TRUE.equals(mPageAnswers.get(origin))));
        if (!ask) return;
        int generation = mGeneration;
        AtomicBoolean done = new AtomicBoolean();
        chrome.onGeolocationPermissionsShowPrompt(origin, (o, allow, retain) -> {
            if (!done.compareAndSet(false, true)) return;
            mProvider.uiHandler().post(() -> {
                if (generation != mGeneration) return;
                if (retain) store.retain(origin, allow);
                mPageAnswers.put(origin, allow);
                List<Runnable> run = mAsking.remove(origin);
                if (run != null) for (Runnable r : run) r.run();
            });
        });
    }

    /**
     * The app asks for a location permission in its manifest (Gesher grants what an app
     * requests, as LocationManagerService checks it), or holds one.
     */
    private static boolean hasLocationPermission(android.content.Context context) {
        try {
            String[] requested = context.getPackageManager().getPackageInfo(
                    context.getPackageName(), PackageManager.GET_PERMISSIONS).requestedPermissions;
            if (requested != null) {
                for (String p : requested) {
                    if (android.Manifest.permission.ACCESS_FINE_LOCATION.equals(p)
                            || android.Manifest.permission.ACCESS_COARSE_LOCATION.equals(p)) {
                        return true;
                    }
                }
            }
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            // ask the context below
        }
        return context.checkSelfPermission(android.Manifest.permission.ACCESS_FINE_LOCATION)
                == PackageManager.PERMISSION_GRANTED
                || context.checkSelfPermission(android.Manifest.permission.ACCESS_COARSE_LOCATION)
                == PackageManager.PERMISSION_GRANTED;
    }

    private LocationManager locationManager() {
        return mProvider.context().getSystemService(LocationManager.class);
    }

    /** The provider a request uses: GPS first for high accuracy, else fused or network. */
    private String provider(LocationManager lm, boolean high) {
        String[] order = high
                ? new String[] {LocationManager.GPS_PROVIDER, LocationManager.FUSED_PROVIDER,
                        LocationManager.NETWORK_PROVIDER}
                : new String[] {LocationManager.FUSED_PROVIDER, LocationManager.NETWORK_PROVIDER,
                        LocationManager.GPS_PROVIDER};
        for (String p : order) {
            try {
                if (lm.isProviderEnabled(p)) return p;
            } catch (RuntimeException e) {
                // not a provider of this device
            }
        }
        return null;
    }

    private void getCurrent(int id, boolean high, long timeout, long maximumAge, int generation) {
        LocationManager lm = locationManager();
        String p = lm != null ? provider(lm, high) : null;
        if (p == null) {
            error(id, POSITION_UNAVAILABLE, "No location provider is enabled");
            return;
        }
        if (maximumAge > 0) {
            try {
                Location last = lm.getLastKnownLocation(p);
                if (last != null && System.currentTimeMillis() - last.getTime() <= maximumAge) {
                    position(id, last);
                    return;
                }
            } catch (SecurityException e) {
                error(id, PERMISSION_DENIED, String.valueOf(e.getMessage()));
                return;
            }
        }
        CancellationSignal signal = new CancellationSignal();
        mGets.add(signal);
        AtomicBoolean done = new AtomicBoolean();
        try {
            lm.getCurrentLocation(p, signal, mProvider.context().getMainExecutor(), loc -> {
                if (!done.compareAndSet(false, true) || generation != mGeneration) return;
                mGets.remove(signal);
                if (loc != null) {
                    position(id, loc);
                } else {
                    error(id, POSITION_UNAVAILABLE, "Position unavailable");
                }
            });
        } catch (SecurityException e) {
            error(id, PERMISSION_DENIED, String.valueOf(e.getMessage()));
            return;
        }
        if (timeout >= 0) {
            mProvider.uiHandler().postDelayed(() -> {
                if (!done.compareAndSet(false, true) || generation != mGeneration) return;
                signal.cancel();
                mGets.remove(signal);
                error(id, TIMEOUT, "Timeout expired");
            }, timeout);
        }
    }

    private void startWatch(int id, boolean high) {
        LocationManager lm = locationManager();
        String p = lm != null ? provider(lm, high) : null;
        if (p == null) {
            error(id, POSITION_UNAVAILABLE, "No location provider is enabled");
            return;
        }
        LocationListener listener = loc -> {
            if (mWatches.containsKey(id)) position(id, loc);
        };
        mWatches.put(id, listener);
        try {
            lm.requestLocationUpdates(p, 1000L, 0f, listener, Looper.myLooper());
            Location last = lm.getLastKnownLocation(p);
            if (last != null) position(id, last);
        } catch (SecurityException e) {
            mWatches.remove(id);
            error(id, PERMISSION_DENIED, String.valueOf(e.getMessage()));
        }
    }

    private void position(int id, Location l) {
        try {
            JSONObject m = new JSONObject();
            m.put("gesher", "geo");
            m.put("id", id);
            m.put("latitude", l.getLatitude());
            m.put("longitude", l.getLongitude());
            m.put("accuracy", l.hasAccuracy() ? l.getAccuracy() : 0.0);
            if (l.hasAltitude()) m.put("altitude", l.getAltitude());
            if (l.hasVerticalAccuracy()) m.put("altitudeAccuracy", l.getVerticalAccuracyMeters());
            if (l.hasBearing()) m.put("heading", l.getBearing());
            if (l.hasSpeed()) m.put("speed", l.getSpeed());
            m.put("timestamp", l.getTime() > 0 ? l.getTime() : System.currentTimeMillis());
            post(m);
        } catch (org.json.JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    private void error(int id, int code, String message) {
        try {
            JSONObject m = new JSONObject();
            m.put("gesher", "geo");
            m.put("id", id);
            m.put("error", code);
            m.put("message", message);
            post(m);
        } catch (org.json.JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    private void post(JSONObject m) {
        WebView2Host host = mProvider.host();
        if (host != null) host.postWebMessage(m.toString());
    }
}
