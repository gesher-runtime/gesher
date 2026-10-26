/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.web3app;

import android.app.Activity;
import android.location.Location;
import android.location.LocationManager;
import android.location.provider.ProviderProperties;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.webkit.GeolocationPermissions;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.LinearLayout;

/**
 * navigator.geolocation in a WebView from LocationManager (ADR 0058), on "Web3 geo" lines:
 * test providers (gps, network, fused) give Jerusalem, the page's getCurrentPosition gets
 * it after the WebChromeClient allowed the origin, its watchPosition then gets Tel Aviv
 * when the test location moves; a second WebView with geolocation disabled gets error 1.
 */
public class GeoActivity extends Activity {
    static final double LAT1 = 31.7683, LON1 = 35.2137, LAT2 = 32.0853, LON2 = 34.7818;

    private LocationManager mLm;
    private final Handler mHandler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mLm = getSystemService(LocationManager.class);
        for (String p : new String[] {LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER,
                LocationManager.FUSED_PROVIDER}) {
            try {
                mLm.addTestProvider(p, new ProviderProperties.Builder().build());
                mLm.setTestProviderEnabled(p, true);
            } catch (RuntimeException e) {
                Web3.log("geo test provider " + p + ": " + e);
            }
        }
        setLocation(LAT1, LON1);
        Web3.log("geo test location set: " + LAT1 + " " + LON1);

        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        WebView allowed = new WebView(this);
        WebView disabled = new WebView(this);
        layout.addView(allowed, new LinearLayout.LayoutParams(-1, 0, 1));
        layout.addView(disabled, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(layout);

        for (WebView w : new WebView[] {allowed, disabled}) {
            w.getSettings().setJavaScriptEnabled(true);
            w.setWebChromeClient(new WebChromeClient() {
                @Override
                public void onGeolocationPermissionsShowPrompt(String origin,
                        GeolocationPermissions.Callback callback) {
                    Web3.log("geo onGeolocationPermissionsShowPrompt: " + origin);
                    callback.invoke(origin, true, false);
                }
            });
        }
        disabled.getSettings().setGeolocationEnabled(false);
        allowed.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                Web3.poll(view, "geoResult.get || geoResult.error || ''", v -> v.length() > 2, 30,
                        got -> {
                            Web3.log("geo getCurrentPosition: " + got);
                            setLocation(LAT2, LON2);
                            Web3.log("geo test location moved: " + LAT2 + " " + LON2);
                            Web3.poll(view, "geoResult.watch",
                                    v -> v.contains(String.format(java.util.Locale.ROOT, "%.4f", LAT2)), 30, watch -> {
                                        Web3.log("geo watchPosition: " + watch);
                                        disabled.setWebViewClient(new WebViewClient() {
                                            @Override
                                            public void onPageFinished(WebView v2, String u2) {
                                                Web3.poll(v2, "geoResult.error || geoResult.get || ''",
                                                        x -> x.length() > 2, 30, e -> {
                                                            Web3.log("geo with geolocation disabled: " + e);
                                                            Web3.log("geo done");
                                                            finish();
                                                        });
                                            }
                                        });
                                        disabled.loadUrl("file:///android_asset/geo.html");
                                    });
                        });
            }
        });
        allowed.loadUrl("file:///android_asset/geo.html");
    }

    private void setLocation(double lat, double lon) {
        for (String p : new String[] {LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER,
                LocationManager.FUSED_PROVIDER}) {
            Location l = new Location(p);
            l.setLatitude(lat);
            l.setLongitude(lon);
            l.setAccuracy(7f);
            l.setAltitude(754);
            l.setTime(System.currentTimeMillis());
            l.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
            try {
                mLm.setTestProviderLocation(p, l);
            } catch (RuntimeException e) {
                Web3.log("geo setTestProviderLocation " + p + ": " + e);
            }
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        for (String p : new String[] {LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER,
                LocationManager.FUSED_PROVIDER}) {
            try {
                mLm.removeTestProvider(p);
            } catch (RuntimeException e) {
                // gone
            }
        }
    }
}
