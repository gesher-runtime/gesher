/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.location.Location;
import android.os.SystemClock;

/**
 * Windows' location service (Windows.Devices.Geolocation.Geolocator, through
 * compat/devices/location_win.cc): the source of {@link LocationManagerService}'s gps,
 * network and fused providers (docs/devices.md). Windows decides whether this desktop
 * program may have the location (Settings, Privacy &amp; security, Location: "Let desktop
 * apps access your location"); without that, or without a location service at all, the
 * providers it backs are disabled.
 */
public final class WindowsLocation {
    /** Geolocator.RequestAccessAsync's answers, and no location service. */
    public static final int UNAVAILABLE = -1;
    public static final int UNSPECIFIED = 0;
    public static final int ALLOWED = 1;
    public static final int DENIED = 2;

    /** Geolocator.LocationStatus: PositionStatus.Disabled, NotAvailable. */
    static final int STATUS_DISABLED = 3;
    static final int STATUS_NOT_AVAILABLE = 5;

    /** PositionSource names, by value (Windows.Devices.Geolocation). */
    private static final String[] SOURCES = {"cellular", "satellite", "wifi", "ip-address",
        "unknown", "default", "obfuscated"};

    private static final long ACCESS_REFRESH_MS = 10_000;

    private static volatile int sAccess = Integer.MIN_VALUE;
    private static volatile int sAccessHr;
    private static volatile int sStatus = -1;
    private static volatile long sAccessAt;
    private static boolean sRefreshing;

    private WindowsLocation() {
    }

    /** A failed Geolocator call, with its HRESULT. */
    public static final class WindowsException extends Exception {
        public final int hr;

        WindowsException(String what, int hr) {
            super(what + ": " + hresult(hr));
            this.hr = hr;
        }
    }

    public static String hresult(int hr) {
        return String.format("HRESULT 0x%08X", hr);
    }

    /**
     * Asks Windows now (RequestAccessAsync, then LocationStatus): may take a moment the
     * first time. Returns the access, one of the constants above.
     */
    public static int refreshAccess() {
        int[] hr = new int[1];
        int access;
        int status = -1;
        try {
            access = nativeAccess(hr);
            if (access != UNAVAILABLE) {
                int[] statusHr = new int[1];
                status = nativeStatus(statusHr);
            }
        } catch (UnsatisfiedLinkError e) {
            access = UNAVAILABLE;
            hr[0] = 0x80004001;  // E_NOTIMPL
        }
        synchronized (WindowsLocation.class) {
            sAccess = access;
            sAccessHr = hr[0];
            sStatus = status;
            sAccessAt = SystemClock.elapsedRealtime();
        }
        return access;
    }

    /** The access last seen; asked again in the background when it is old. */
    static int access() {
        if (sAccess == Integer.MIN_VALUE) return refreshAccess();
        boolean refresh = false;
        synchronized (WindowsLocation.class) {
            if (!sRefreshing && SystemClock.elapsedRealtime() - sAccessAt > ACCESS_REFRESH_MS) {
                sRefreshing = true;
                refresh = true;
            }
        }
        if (refresh) {
            Thread t = new Thread(() -> {
                try {
                    refreshAccess();
                } finally {
                    synchronized (WindowsLocation.class) {
                        sRefreshing = false;
                    }
                }
            }, "windows-location-access");
            t.setDaemon(true);
            t.start();
        }
        return sAccess;
    }

    /** Whether Windows gives positions to this program now. */
    static boolean enabled() {
        return access() == ALLOWED && sStatus != STATUS_DISABLED
                && sStatus != STATUS_NOT_AVAILABLE;
    }

    /** "allowed", "denied (HRESULT ...)" and so on, with the location status. */
    public static String describe() {
        int access = sAccess == Integer.MIN_VALUE ? refreshAccess() : sAccess;
        String name;
        switch (access) {
            case ALLOWED: name = "allowed"; break;
            case DENIED: name = "denied"; break;
            case UNSPECIFIED: name = "unspecified"; break;
            default: name = "unavailable (" + hresult(sAccessHr) + ")"; break;
        }
        return name + ", status " + statusName(sStatus);
    }

    static String statusName(int status) {
        switch (status) {
            case 0: return "ready";
            case 1: return "initializing";
            case 2: return "no data";
            case 3: return "disabled";
            case 4: return "not initialized";
            case 5: return "not available";
            default: return "unknown";
        }
    }

    /**
     * One position from Windows (blocks up to {@code timeoutMs}), as a Location of
     * {@code provider}. {@code high}: DesiredAccuracy High (satellites when there are any).
     */
    public static Location fix(String provider, boolean high, long maxAgeMs, long timeoutMs)
            throws WindowsException {
        double[] v = new double[9];
        int hr;
        try {
            hr = nativeGetFix(high, maxAgeMs, timeoutMs, v);
        } catch (UnsatisfiedLinkError e) {
            throw new WindowsException("Geolocator", 0x80004001);
        }
        if (hr < 0) throw new WindowsException("Geolocator.GetGeopositionAsync", hr);
        Location l = new Location(provider);
        l.setLatitude(v[0]);
        l.setLongitude(v[1]);
        l.setAccuracy((float) Math.max(v[2], 0));
        if (!Double.isNaN(v[3])) l.setAltitude(v[3]);
        if (!Double.isNaN(v[4])) l.setVerticalAccuracyMeters((float) v[4]);
        if (!Double.isNaN(v[5])) l.setBearing((float) v[5]);
        if (!Double.isNaN(v[6])) l.setSpeed((float) v[6]);
        long now = System.currentTimeMillis();
        long time = v[7] > 0 ? (long) v[7] : now;
        l.setTime(time);
        // The fix's age on the elapsed-realtime clock (never in the future).
        long age = Math.max(0, now - time);
        l.setElapsedRealtimeNanos(Math.max(0,
                SystemClock.elapsedRealtimeNanos() - age * 1_000_000L));
        int source = (int) v[8];
        if (source >= 0 && source < SOURCES.length) {
            android.os.Bundle extras = new android.os.Bundle();
            extras.putString("windowsPositionSource", SOURCES[source]);
            l.setExtras(extras);
        }
        return l;
    }

    private static native int nativeAccess(int[] hr);

    private static native int nativeStatus(int[] hr);

    private static native int nativeGetFix(boolean high, long maxAgeMs, long timeoutMs,
            double[] out);
}
