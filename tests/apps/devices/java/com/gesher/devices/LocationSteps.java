/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devices;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.location.Criteria;
import android.location.Geocoder;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.location.LocationRequest;
import android.location.provider.ProviderProperties;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * LocationManager end to end: the providers, a test provider replacing gps (listeners,
 * a PendingIntent, last and current locations, enabled and disabled), fused the way apps
 * without Play services ask for it (LocationRequest and an Executor), and Windows' own
 * network provider, whatever this machine allows.
 */
final class LocationSteps {
    static final String ACTION = "com.gesher.devices.LOCATION";

    private final Context mContext;
    private final LocationManager mLm;
    private final HandlerThread mThread = new HandlerThread("location-callbacks");
    private final Handler mHandler;
    private final Executor mExecutor;
    private final BlockingQueue<String> mEvents = new LinkedBlockingQueue<>();

    LocationSteps(Context context) {
        mContext = context;
        mLm = context.getSystemService(LocationManager.class);
        mThread.start();
        mHandler = new Handler(mThread.getLooper());
        mExecutor = mHandler::post;
    }

    private static String format(Location l) {
        if (l == null) return "null";
        return String.format(Locale.ROOT, "%s %.4f,%.4f acc %.0f mock %b", l.getProvider(),
                l.getLatitude(), l.getLongitude(), l.getAccuracy(), l.isMock());
    }

    private static Location at(String provider, double lat, double lon) {
        Location l = new Location(provider);
        l.setLatitude(lat);
        l.setLongitude(lon);
        l.setAccuracy(5);
        l.setAltitude(800);
        l.setTime(System.currentTimeMillis());
        l.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
        return l;
    }

    private String next(long timeoutMs) throws InterruptedException {
        String e = mEvents.poll(timeoutMs, TimeUnit.MILLISECONDS);
        return e != null ? e : "(nothing within " + timeoutMs + " ms)";
    }

    private final LocationListener mListener = new LocationListener() {
        @Override
        public void onLocationChanged(Location location) {
            mEvents.add("listener " + format(location));
        }

        @Override
        public void onProviderEnabled(String provider) {
            mEvents.add("listener onProviderEnabled " + provider);
        }

        @Override
        public void onProviderDisabled(String provider) {
            mEvents.add("listener onProviderDisabled " + provider);
        }
    };

    void run() throws Exception {
        Report.line("location: providers " + mLm.getAllProviders() + ", location enabled "
                + mLm.isLocationEnabled() + ", Geocoder.isPresent " + Geocoder.isPresent());
        Report.line("location: Windows-backed network provider enabled "
                + mLm.isProviderEnabled(LocationManager.NETWORK_PROVIDER));

        // A test provider in gps' place.
        ProviderProperties props = new ProviderProperties.Builder()
                .setHasSatelliteRequirement(true)
                .setAccuracy(ProviderProperties.ACCURACY_FINE)
                .setPowerUsage(ProviderProperties.POWER_USAGE_HIGH)
                .build();
        String gps = LocationManager.GPS_PROVIDER;
        mLm.addTestProvider(gps, props);
        Report.line("location: test gps added, enabled " + mLm.isProviderEnabled(gps));
        mLm.setTestProviderEnabled(gps, true);
        Report.line("location: test gps enabled " + mLm.isProviderEnabled(gps));

        // A registered receiver for the PendingIntent (targeted to this package).
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Location l = intent.getParcelableExtra(LocationManager.KEY_LOCATION_CHANGED,
                        Location.class);
                if (l != null) mEvents.add("pending intent " + format(l));
            }
        };
        mContext.registerReceiver(receiver, new IntentFilter(ACTION), null, mHandler,
                Context.RECEIVER_NOT_EXPORTED);
        PendingIntent pi = PendingIntent.getBroadcast(mContext, 1,
                new Intent(ACTION).setPackage(mContext.getPackageName()),
                PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        mLm.requestLocationUpdates(gps, 0, 0, mListener, mThread.getLooper());
        mLm.requestLocationUpdates(gps, new LocationRequest.Builder(0).build(), pi);
        mLm.setTestProviderLocation(gps, at(gps, 31.7767, 35.2345));
        // The listener first, then the PendingIntent's broadcast (a registered receiver: in
        // the single-process mode there is none, as the process is not attached).
        String a = next(10000);
        String b = next(a.startsWith("listener") || a.startsWith("pending") ? 5000 : 10000);
        String listener = a.startsWith("listener") ? a : b.startsWith("listener") ? b : null;
        String pending = a.startsWith("pending") ? a : b.startsWith("pending") ? b : null;
        Report.line("location: update to the listener "
                + (listener != null ? listener : "missing (" + a + "; " + b + ")"));
        Report.line("location: update to the PendingIntent "
                + (pending != null ? pending : "missing (" + a + "; " + b + ")"));
        Report.line("location: getLastKnownLocation gps " + format(mLm.getLastKnownLocation(gps))
                + ", passive " + format(mLm.getLastKnownLocation(LocationManager.PASSIVE_PROVIDER)));

        // getCurrentLocation: the fresh last location at once.
        mLm.getCurrentLocation(gps, null, mExecutor, l -> mEvents.add("current " + format(l)));
        Report.line("location: getCurrentLocation " + next(10000));

        // Removing the PendingIntent; the listener still gets the next one.
        mLm.removeUpdates(pi);
        mLm.setTestProviderLocation(gps, at(gps, 32.0853, 34.7818));
        Report.line("location: after removeUpdates(PendingIntent) " + next(10000));
        Report.line("location: and nothing else " + next(1000));

        // Disabled and enabled again: the listener hears of both.
        mLm.setTestProviderEnabled(gps, false);
        Report.line("location: test gps disabled: " + next(10000) + ", enabled "
                + mLm.isProviderEnabled(gps));
        mLm.setTestProviderEnabled(gps, true);
        Report.line("location: test gps enabled again: " + next(10000));
        mLm.removeUpdates(mListener);
        mLm.setTestProviderLocation(gps, at(gps, 31.0, 35.0));
        Report.line("location: after removeUpdates(listener) " + next(1000));

        // Fused, as androidx's LocationManagerCompat asks without Play services.
        String fused = LocationManager.FUSED_PROVIDER;
        mLm.addTestProvider(fused, props);
        mLm.setTestProviderEnabled(fused, true);
        LocationRequest request = new LocationRequest.Builder(1000)
                .setQuality(LocationRequest.QUALITY_HIGH_ACCURACY).setMaxUpdates(1).build();
        mLm.requestLocationUpdates(fused, request, mExecutor,
                l -> mEvents.add("fused " + format(l)));
        mLm.setTestProviderLocation(fused, at(fused, 29.5581, 34.9482));
        Report.line("location: fused via LocationRequest and an Executor " + next(10000));
        Report.line("location: best provider " + mLm.getBestProvider(new Criteria(), true));
        mLm.removeTestProvider(fused);
        mLm.removeTestProvider(gps);
        Report.line("location: test providers removed, gps enabled "
                + mLm.isProviderEnabled(gps) + " (Windows)");
        mContext.unregisterReceiver(receiver);

        // Geocoder: none.
        try {
            new Geocoder(mContext).getFromLocation(31.7767, 35.2345, 1);
            Report.line("location: Geocoder answered");
        } catch (java.io.IOException e) {
            Report.line("location: Geocoder throws IOException (no geocoder)");
        }

        // Windows' own provider: a position if this machine gives one, else its reason.
        if (mLm.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
            mLm.getCurrentLocation(LocationManager.NETWORK_PROVIDER, null, mExecutor,
                    l -> mEvents.add("windows " + (l == null ? "no position" : String.format(
                            Locale.ROOT, "a position, accuracy %.0f m", l.getAccuracy()))));
            Report.line("location: Windows network provider: " + next(40000));
        } else {
            Report.line("location: Windows network provider: disabled (Windows gives this"
                    + " program no location)");
        }
        Report.line("location: done");
        mThread.quitSafely();
    }
}
