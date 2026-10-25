/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.PendingIntent;
import android.content.IIntentSender;
import android.content.Intent;
import android.location.Criteria;
import android.location.GnssCapabilities;
import android.location.ILocationCallback;
import android.location.ILocationListener;
import android.location.ILocationManager;
import android.location.LastLocationRequest;
import android.location.Location;
import android.location.LocationManager;
import android.location.LocationRequest;
import android.location.provider.ForwardGeocodeRequest;
import android.location.provider.IGeocodeCallback;
import android.location.provider.ProviderProperties;
import android.location.provider.ReverseGeocodeRequest;
import android.os.Binder;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ICancellationSignal;
import android.os.IBinder;
import android.os.PackageTagsList;
import android.os.RemoteException;
import android.os.SystemClock;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/**
 * {@code location}: LocationManagerService on Windows (docs/devices.md, ADR 0043). The
 * gps, network and fused providers are Windows' location service ({@link WindowsLocation});
 * passive gets what the others report. Test providers (addTestProvider and
 * setTestProviderLocation) replace a provider of the same name, or add one, as on Android.
 *
 * <p>Permissions: an app needs ACCESS_FINE_LOCATION or ACCESS_COARSE_LOCATION in its
 * manifest (Gesher grants what an app requests, docs/devices.md); with coarse only, it
 * gets coarse locations (snapped to a 2 km grid). Windows' own setting decides for every
 * app at once: when it denies this program the location, the Windows-backed providers are
 * disabled.
 *
 * <p>Requests: listeners and PendingIntents (KEY_LOCATION_CHANGED, KEY_PROVIDER_ENABLED,
 * KEY_FLUSH_COMPLETE), getCurrentLocation, getLastLocation; the interval, minimum update
 * interval and distance, maximum updates and duration of a LocationRequest are honoured.
 * Windows is polled at the shortest interval asked for (at least a second). No geocoder
 * (Geocoder.isPresent() is false), no GNSS status or measurements, no geofences.
 */
public final class LocationManagerService extends ILocationManager.Default {
    private static final String TAG = "location";
    /** The real providers, in getAllProviders' order. */
    private static final String[] REAL = {LocationManager.PASSIVE_PROVIDER,
        LocationManager.NETWORK_PROVIDER, LocationManager.FUSED_PROVIDER,
        LocationManager.GPS_PROVIDER};
    private static final long MIN_POLL_MS = 1000;
    /** getCurrentLocation takes a last location this fresh (as Android: 30 s). */
    private static final long CURRENT_MAX_AGE_MS = 30_000;
    /** Coarse locations: snapped to a grid of this size (Android's LocationFudger: 2 km). */
    private static final double COARSE_GRID_M = 2000;
    private static final int PERMISSION_NONE = 0;
    private static final int PERMISSION_COARSE = 1;
    private static final int PERMISSION_FINE = 2;

    private final PackageManagerService mPm;
    private final ActivityManagerService mAm;
    private final Object mLock = new Object();
    private final Map<String, TestProvider> mTestProviders = new LinkedHashMap<>();
    private final Map<String, Location> mLast = new HashMap<>();
    private final List<Registration> mRegistrations = new ArrayList<>();
    private final ScheduledExecutorService mExecutor = Executors.newSingleThreadScheduledExecutor(
            r -> {
                Thread t = new Thread(r, "location");
                t.setDaemon(true);
                return t;
            });
    private ScheduledFuture<?> mPoll;
    private long mPollInterval;
    /** The enabled state each provider last reported to the registrations. */
    private final Map<String, Boolean> mReportedEnabled = new HashMap<>();

    public LocationManagerService(PackageManagerService pm, ActivityManagerService am) {
        mPm = pm;
        mAm = am;
        // Windows' answer may take a moment: asked now, off the binder threads.
        mExecutor.execute(() -> {
            WindowsLocation.refreshAccess();
            log("Windows location: " + WindowsLocation.describe());
            checkEnabledChanges();
        });
    }

    static void log(String message) {
        SystemServer.log(TAG + ": " + message);
    }

    // --- providers -----------------------------------------------------------------------

    private static final class TestProvider {
        final String name;
        final ProviderProperties properties;
        boolean enabled;

        TestProvider(String name, ProviderProperties properties) {
            this.name = name;
            this.properties = properties;
        }
    }

    private static boolean isReal(String provider) {
        for (String p : REAL) {
            if (p.equals(provider)) return true;
        }
        return false;
    }

    private boolean exists(String provider) {
        synchronized (mLock) {
            return isReal(provider) || mTestProviders.containsKey(provider);
        }
    }

    private void checkProvider(String provider) {
        if (provider == null || !exists(provider)) {
            throw new IllegalArgumentException("provider \"" + provider + "\" does not exist");
        }
    }

    /** Whether Windows (or a test provider standing in for it) gives this provider fixes. */
    private boolean isEnabled(String provider) {
        TestProvider test;
        synchronized (mLock) {
            test = mTestProviders.get(provider);
        }
        if (test != null) return test.enabled;
        if (!isReal(provider)) return false;
        if (LocationManager.PASSIVE_PROVIDER.equals(provider)) return isLocationEnabled();
        return WindowsLocation.enabled();
    }

    /** Location is on when Windows gives it to this program, or a test provider is on. */
    private boolean isLocationEnabled() {
        synchronized (mLock) {
            for (TestProvider t : mTestProviders.values()) {
                if (t.enabled) return true;
            }
        }
        return WindowsLocation.enabled();
    }

    private static ProviderProperties realProperties(String provider) {
        boolean gps = LocationManager.GPS_PROVIDER.equals(provider);
        return new ProviderProperties.Builder()
                .setHasNetworkRequirement(!gps)
                .setHasSatelliteRequirement(gps)
                .setHasAltitudeSupport(true)
                .setHasSpeedSupport(true)
                .setHasBearingSupport(true)
                .setPowerUsage(gps ? ProviderProperties.POWER_USAGE_HIGH
                        : ProviderProperties.POWER_USAGE_LOW)
                .setAccuracy(gps ? ProviderProperties.ACCURACY_FINE
                        : ProviderProperties.ACCURACY_COARSE)
                .build();
    }

    @Override
    public boolean hasProvider(String provider) {
        return provider != null && exists(provider);
    }

    @Override
    public List<String> getAllProviders() {
        List<String> all = new ArrayList<>();
        for (String p : REAL) all.add(p);
        synchronized (mLock) {
            for (String p : mTestProviders.keySet()) {
                if (!all.contains(p)) all.add(p);
            }
        }
        return all;
    }

    @Override
    public List<String> getProviders(Criteria criteria, boolean enabledOnly) {
        List<String> result = new ArrayList<>();
        for (String p : getAllProviders()) {
            if (enabledOnly && !isEnabled(p)) continue;
            if (criteria != null && !matches(p, criteria)) continue;
            result.add(p);
        }
        return result;
    }

    private boolean matches(String provider, Criteria criteria) {
        ProviderProperties props = getProviderProperties(provider);
        if (props == null) return LocationManager.PASSIVE_PROVIDER.equals(provider);
        if (criteria.getAccuracy() == Criteria.ACCURACY_FINE
                && props.getAccuracy() != ProviderProperties.ACCURACY_FINE) {
            return false;
        }
        if (criteria.getPowerRequirement() != Criteria.NO_REQUIREMENT
                && props.getPowerUsage() > criteria.getPowerRequirement()) {
            return false;
        }
        if (criteria.isAltitudeRequired() && !props.hasAltitudeSupport()) return false;
        if (criteria.isSpeedRequired() && !props.hasSpeedSupport()) return false;
        if (criteria.isBearingRequired() && !props.hasBearingSupport()) return false;
        return criteria.isCostAllowed() || !props.hasMonetaryCost();
    }

    @Override
    public String getBestProvider(Criteria criteria, boolean enabledOnly) {
        List<String> providers = getProviders(criteria, enabledOnly);
        providers.remove(LocationManager.PASSIVE_PROVIDER);
        if (providers.isEmpty() && criteria != null) {
            providers = getProviders(null, enabledOnly);
            providers.remove(LocationManager.PASSIVE_PROVIDER);
        }
        for (String p : new String[] {LocationManager.FUSED_PROVIDER,
                LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER}) {
            if (providers.contains(p)) return p;
        }
        return providers.isEmpty() ? null : providers.get(0);
    }

    @Override
    public ProviderProperties getProviderProperties(String provider) {
        checkProvider(provider);
        synchronized (mLock) {
            TestProvider t = mTestProviders.get(provider);
            if (t != null) return t.properties;
        }
        return LocationManager.PASSIVE_PROVIDER.equals(provider) ? null
                : realProperties(provider);
    }

    @Override
    public boolean isProviderEnabledForUser(String provider, int userId) {
        return provider != null && exists(provider) && isEnabled(provider);
    }

    @Override
    public boolean isLocationEnabledForUser(int userId) {
        return isLocationEnabled();
    }

    @Override
    public void setLocationEnabledForUser(boolean enabled, int userId) {
        // Windows' setting decides; apps cannot change it.
        log("setLocationEnabledForUser(" + enabled + ") ignored: Windows' location setting"
                + " decides");
    }

    @Override
    public boolean isAdasGnssLocationEnabledForUser(int userId) {
        return false;
    }

    @Override
    public boolean isProviderPackage(String provider, String packageName, String attributionTag) {
        return false;
    }

    @Override
    public List<String> getProviderPackages(String provider) {
        return Collections.emptyList();
    }

    @Override
    public String getExtraLocationControllerPackage() {
        return null;
    }

    @Override
    public boolean isExtraLocationControllerPackageEnabled() {
        return false;
    }

    @Override
    public String[] getBackgroundThrottlingWhitelist() {
        return new String[0];
    }

    @Override
    public PackageTagsList getIgnoreSettingsAllowlist() {
        return new PackageTagsList.Builder().build();
    }

    @Override
    public PackageTagsList getAdasAllowlist() {
        return new PackageTagsList.Builder().build();
    }

    // --- permissions ---------------------------------------------------------------------

    /** The location permission {@code packageName} holds: what its manifest asks for. */
    private int permissionLevel(String packageName) {
        ApkPackage p = mPm.get(packageName);
        if (p == null) {
            // Not an installed package (a tool on the device's own uid): the system's own.
            return packageName == null || "android".equals(packageName) ? PERMISSION_FINE
                    : PERMISSION_NONE;
        }
        java.util.Collection<String> requested = p.parsed.getRequestedPermissions();
        if (requested.contains(Manifest.permission.ACCESS_FINE_LOCATION)) return PERMISSION_FINE;
        if (requested.contains(Manifest.permission.ACCESS_COARSE_LOCATION)) {
            return PERMISSION_COARSE;
        }
        return PERMISSION_NONE;
    }

    private int enforceLocationPermission(String packageName) {
        int level = permissionLevel(packageName);
        if (level == PERMISSION_NONE) {
            throw new SecurityException("uid " + Binder.getCallingUid() + " (" + packageName
                    + ") does not have " + Manifest.permission.ACCESS_COARSE_LOCATION + " or "
                    + Manifest.permission.ACCESS_FINE_LOCATION + ".");
        }
        return level;
    }

    /** What an app with {@code level} may see of {@code location}. */
    private static Location forPermission(Location location, int level) {
        if (location == null) return null;
        Location l = new Location(location);
        if (level == PERMISSION_FINE) return l;
        // Coarse: snapped to a grid (as LocationFudger), no altitude, speed or bearing.
        double latGrid = COARSE_GRID_M / 111_320.0;
        double lat = Math.round(l.getLatitude() / latGrid) * latGrid;
        double cos = Math.max(Math.cos(Math.toRadians(lat)), 0.01);
        double lonGrid = latGrid / cos;
        double lon = Math.round(l.getLongitude() / lonGrid) * lonGrid;
        l.setLatitude(Math.max(-90, Math.min(90, lat)));
        l.setLongitude(((lon + 540) % 360) - 180);
        l.setAccuracy((float) Math.max(l.hasAccuracy() ? l.getAccuracy() : 0, COARSE_GRID_M));
        l.removeAltitude();
        l.removeSpeed();
        l.removeBearing();
        l.removeVerticalAccuracy();
        l.removeSpeedAccuracy();
        l.removeBearingAccuracy();
        return l;
    }

    // --- last and current locations ------------------------------------------------------

    @Override
    public Location getLastLocation(String provider, LastLocationRequest request,
            String packageName, String attributionTag) {
        int level = enforceLocationPermission(packageName);
        checkProvider(provider);
        if (!isEnabled(provider)) return null;
        Location last;
        synchronized (mLock) {
            last = mLast.get(provider);
        }
        if (last == null && !isTest(provider) && !LocationManager.PASSIVE_PROVIDER.equals(provider)
                && WindowsLocation.enabled()) {
            // Windows keeps a last position too: a cached one, if any, quickly.
            try {
                last = WindowsLocation.fix(provider, false, 24L * 3600_000, 1000);
                report(provider, last, false);
            } catch (WindowsLocation.WindowsException e) {
                log("no last location from Windows: " + e.getMessage());
            }
        }
        return forPermission(last, level);
    }

    private boolean isTest(String provider) {
        synchronized (mLock) {
            return mTestProviders.containsKey(provider);
        }
    }

    @Override
    public ICancellationSignal getCurrentLocation(String provider, LocationRequest request,
            ILocationCallback callback, String packageName, String attributionTag,
            String listenerId) {
        int level = enforceLocationPermission(packageName);
        checkProvider(provider);
        ICancellationSignal transport = CancellationSignal.createTransport();
        CancellationSignal cancel = CancellationSignal.fromTransport(transport);
        if (!isEnabled(provider)) {
            deliverCurrent(callback, null);
            return transport;
        }
        Location last;
        synchronized (mLock) {
            last = mLast.get(provider);
        }
        if (last != null && ageMillis(last) <= CURRENT_MAX_AGE_MS) {
            deliverCurrent(callback, forPermission(last, level));
            return transport;
        }
        // The next location of the provider, within the request's duration.
        long duration = Math.min(request.getDurationMillis(), 30_000);
        Registration r = new Registration(provider, request, packageName, level);
        r.current = callback;
        r.maxUpdates = 1;
        r.expiresAt = SystemClock.elapsedRealtime() + duration;
        add(r);
        cancel.setOnCancelListener(() -> remove(r));
        mExecutor.schedule(() -> {
            if (remove(r)) deliverCurrent(callback, null);
        }, duration, TimeUnit.MILLISECONDS);
        return transport;
    }

    private static void deliverCurrent(ILocationCallback callback, Location location) {
        try {
            callback.onLocation(location);
        } catch (RemoteException e) {
            // gone
        }
    }

    private static long ageMillis(Location l) {
        return (SystemClock.elapsedRealtimeNanos() - l.getElapsedRealtimeNanos()) / 1_000_000L;
    }

    // --- registrations -------------------------------------------------------------------

    /** A listener's, a PendingIntent's or getCurrentLocation's request. */
    private final class Registration implements IBinder.DeathRecipient {
        final String provider;
        final LocationRequest request;
        final String packageName;
        final int level;
        ILocationListener listener;
        PendingIntent pendingIntent;
        ILocationCallback current;
        int maxUpdates;
        int updates;
        long expiresAt = Long.MAX_VALUE;
        Location lastDelivered;

        Registration(String provider, LocationRequest request, String packageName, int level) {
            this.provider = provider;
            this.request = request;
            this.packageName = packageName;
            this.level = level;
            this.maxUpdates = request.getMaxUpdates();
            long duration = request.getDurationMillis();
            if (duration != Long.MAX_VALUE) {
                expiresAt = SystemClock.elapsedRealtime() + duration;
            }
        }

        boolean sameClient(ILocationListener l) {
            return listener != null && l != null && listener.asBinder() == l.asBinder();
        }

        @Override
        public void binderDied() {
            remove(this);
        }

        /** Delivers {@code location} if the request wants it now; false once it is done. */
        boolean deliver(Location location) {
            long now = SystemClock.elapsedRealtime();
            if (now > expiresAt) return false;
            if (lastDelivered != null) {
                long minInterval = request.getMinUpdateIntervalMillis();
                long since = (location.getElapsedRealtimeNanos()
                        - lastDelivered.getElapsedRealtimeNanos()) / 1_000_000L;
                if (since < minInterval - Math.min(minInterval / 10, 30_000)) return true;
                if (lastDelivered.distanceTo(location) < request.getMinUpdateDistanceMeters()) {
                    return true;
                }
            }
            Location l = forPermission(location, level);
            if (!provider.equals(l.getProvider())) l.setProvider(provider);
            lastDelivered = location;
            updates++;
            try {
                if (current != null) {
                    current.onLocation(l);
                } else if (listener != null) {
                    listener.onLocationChanged(List.of(l), null);
                } else if (pendingIntent != null) {
                    Intent fillIn = new Intent();
                    fillIn.putExtra(LocationManager.KEY_LOCATION_CHANGED, l);
                    fillIn.putExtra(LocationManager.KEY_LOCATIONS, new Location[] {l});
                    send(pendingIntent, fillIn);
                }
            } catch (RemoteException | RuntimeException e) {
                log("delivering to " + packageName + " failed: " + e);
                return false;
            }
            return updates < maxUpdates;
        }

        void providerEnabledChanged(boolean enabled) {
            try {
                if (listener != null) {
                    listener.onProviderEnabledChanged(provider, enabled);
                } else if (pendingIntent != null) {
                    Intent fillIn = new Intent();
                    fillIn.putExtra(LocationManager.KEY_PROVIDER_ENABLED, enabled);
                    send(pendingIntent, fillIn);
                }
            } catch (RemoteException | RuntimeException e) {
                log("provider change to " + packageName + " failed: " + e);
            }
        }
    }

    private static void send(PendingIntent pendingIntent, Intent fillIn) throws RemoteException {
        IIntentSender target = pendingIntent.getTarget();
        target.send(0, fillIn, null, null, null, null, null);
    }

    private void add(Registration r) {
        synchronized (mLock) {
            mRegistrations.add(r);
        }
        if (r.listener != null) {
            try {
                r.listener.asBinder().linkToDeath(r, 0);
            } catch (RemoteException e) {
                remove(r);
                return;
            }
        }
        updatePolling();
    }

    private boolean remove(Registration r) {
        boolean removed;
        synchronized (mLock) {
            removed = mRegistrations.remove(r);
        }
        if (removed) {
            if (r.listener != null) {
                try {
                    r.listener.asBinder().unlinkToDeath(r, 0);
                } catch (RuntimeException e) {
                    // not linked
                }
            }
            updatePolling();
        }
        return removed;
    }

    @Override
    public void registerLocationListener(String provider, LocationRequest request,
            ILocationListener listener, String packageName, String attributionTag,
            String listenerId) {
        int level = enforceLocationPermission(packageName);
        checkProvider(provider);
        unregisterLocationListener(listener);
        Registration r = new Registration(provider, request, packageName, level);
        r.listener = listener;
        add(r);
        log("registerLocationListener " + provider + " for " + packageName + ": interval "
                + request.getIntervalMillis() + " ms");
        if (!isEnabled(provider)) r.providerEnabledChanged(false);
    }

    @Override
    public void unregisterLocationListener(ILocationListener listener) {
        List<Registration> gone = new ArrayList<>();
        synchronized (mLock) {
            for (Registration r : mRegistrations) {
                if (r.sameClient(listener)) gone.add(r);
            }
        }
        for (Registration r : gone) {
            remove(r);
            log("unregisterLocationListener " + r.provider + " for " + r.packageName);
        }
    }

    @Override
    public void registerLocationPendingIntent(String provider, LocationRequest request,
            PendingIntent pendingIntent, String packageName, String attributionTag) {
        int level = enforceLocationPermission(packageName);
        checkProvider(provider);
        unregisterLocationPendingIntent(pendingIntent);
        Registration r = new Registration(provider, request, packageName, level);
        r.pendingIntent = pendingIntent;
        add(r);
        log("registerLocationPendingIntent " + provider + " for " + packageName);
        if (!isEnabled(provider)) r.providerEnabledChanged(false);
    }

    @Override
    public void unregisterLocationPendingIntent(PendingIntent pendingIntent) {
        List<Registration> gone = new ArrayList<>();
        synchronized (mLock) {
            for (Registration r : mRegistrations) {
                if (r.pendingIntent != null && r.pendingIntent.equals(pendingIntent)) gone.add(r);
            }
        }
        for (Registration r : gone) {
            remove(r);
            log("unregisterLocationPendingIntent " + r.provider + " for " + r.packageName);
        }
    }

    @Override
    public void requestListenerFlush(String provider, ILocationListener listener,
            int requestCode) {
        try {
            listener.onFlushComplete(requestCode);
        } catch (RemoteException e) {
            // gone
        }
    }

    @Override
    public void requestPendingIntentFlush(String provider, PendingIntent pendingIntent,
            int requestCode) {
        Intent fillIn = new Intent();
        fillIn.putExtra(LocationManager.KEY_FLUSH_COMPLETE, requestCode);
        try {
            send(pendingIntent, fillIn);
        } catch (RemoteException e) {
            // gone
        }
    }

    // --- reporting -----------------------------------------------------------------------

    /** A new location of {@code provider}: stored, then to its registrations and passive's. */
    private void report(String provider, Location location, boolean deliver) {
        location = new Location(location);
        location.setProvider(provider);
        synchronized (mLock) {
            mLast.put(provider, location);
            mLast.put(LocationManager.PASSIVE_PROVIDER, location);
        }
        if (!deliver) return;
        List<Registration> targets = new ArrayList<>();
        synchronized (mLock) {
            for (Registration r : mRegistrations) {
                if (r.provider.equals(provider)
                        || r.provider.equals(LocationManager.PASSIVE_PROVIDER)) {
                    targets.add(r);
                }
            }
        }
        for (Registration r : targets) {
            if (!r.deliver(location)) remove(r);
        }
    }

    /** Polls Windows for the registrations on its providers, at their shortest interval. */
    private void updatePolling() {
        long interval = Long.MAX_VALUE;
        synchronized (mLock) {
            for (Registration r : mRegistrations) {
                if (!isReal(r.provider) || mTestProviders.containsKey(r.provider)
                        || LocationManager.PASSIVE_PROVIDER.equals(r.provider)) {
                    continue;
                }
                long i = r.current != null ? 0 : r.request.getIntervalMillis();
                interval = Math.min(interval, Math.max(i, MIN_POLL_MS));
            }
            if (interval == mPollInterval && mPoll != null) return;
            if (mPoll != null) {
                mPoll.cancel(false);
                mPoll = null;
            }
            mPollInterval = interval;
            if (interval != Long.MAX_VALUE) {
                mPoll = mExecutor.scheduleWithFixedDelay(this::poll, 0, interval,
                        TimeUnit.MILLISECONDS);
            }
        }
    }

    private void poll() {
        checkEnabledChanges();
        if (!WindowsLocation.enabled()) return;
        boolean high = false;
        List<String> providers = new ArrayList<>();
        synchronized (mLock) {
            for (Registration r : mRegistrations) {
                if (!isReal(r.provider) || mTestProviders.containsKey(r.provider)
                        || LocationManager.PASSIVE_PROVIDER.equals(r.provider)) {
                    continue;
                }
                if (!providers.contains(r.provider)) providers.add(r.provider);
                high |= r.request.getQuality() == LocationRequest.QUALITY_HIGH_ACCURACY
                        || LocationManager.GPS_PROVIDER.equals(r.provider);
            }
        }
        if (providers.isEmpty()) return;
        Location fix;
        try {
            fix = WindowsLocation.fix(providers.get(0), high, mPollInterval,
                    Math.min(Math.max(mPollInterval, 5000), 30_000));
        } catch (WindowsLocation.WindowsException e) {
            log("Windows gave no position: " + e.getMessage());
            return;
        }
        for (String p : providers) report(p, fix, true);
    }

    /** Tells registrations about providers turned on or off since the last check. */
    private void checkEnabledChanges() {
        List<String> providers = getAllProviders();
        for (String p : providers) {
            boolean enabled = isEnabled(p);
            Boolean before;
            synchronized (mLock) {
                before = mReportedEnabled.put(p, enabled);
            }
            if (before == null || before == enabled) continue;
            log("provider " + p + (enabled ? " enabled" : " disabled"));
            List<Registration> targets = new ArrayList<>();
            synchronized (mLock) {
                for (Registration r : mRegistrations) {
                    if (r.provider.equals(p)) targets.add(r);
                }
            }
            for (Registration r : targets) r.providerEnabledChanged(enabled);
            Intent changed = new Intent(LocationManager.PROVIDERS_CHANGED_ACTION);
            changed.putExtra(LocationManager.EXTRA_PROVIDER_NAME, p);
            changed.putExtra(LocationManager.EXTRA_PROVIDER_ENABLED, enabled);
            changed.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY);
            try {
                mAm.sendBroadcastFor("android", changed, null);
            } catch (RuntimeException e) {
                log("PROVIDERS_CHANGED broadcast failed: " + e);
            }
        }
    }

    // --- test providers ------------------------------------------------------------------

    @Override
    public void addTestProvider(String name, ProviderProperties properties,
            List<String> locationTags, String packageName, String attributionTag) {
        if (name == null || LocationManager.PASSIVE_PROVIDER.equals(name)) {
            throw new IllegalArgumentException("cannot mock the passive provider");
        }
        synchronized (mLock) {
            mTestProviders.put(name, new TestProvider(name, properties));
            mLast.remove(name);
        }
        log("addTestProvider " + name + " by " + packageName);
        updatePolling();
        checkEnabledChanges();
    }

    @Override
    public void removeTestProvider(String provider, String packageName, String attributionTag) {
        TestProvider removed;
        synchronized (mLock) {
            removed = mTestProviders.remove(provider);
            if (removed != null) mLast.remove(provider);
        }
        if (removed == null) {
            throw new IllegalArgumentException("provider \"" + provider + "\" is not a test"
                    + " provider");
        }
        log("removeTestProvider " + provider);
        if (!isReal(provider)) {
            // Its registrations end with it.
            List<Registration> gone = new ArrayList<>();
            synchronized (mLock) {
                for (Iterator<Registration> it = mRegistrations.iterator(); it.hasNext(); ) {
                    Registration r = it.next();
                    if (r.provider.equals(provider)) gone.add(r);
                }
            }
            for (Registration r : gone) remove(r);
        }
        updatePolling();
        checkEnabledChanges();
    }

    @Override
    public void setTestProviderLocation(String provider, Location location, String packageName,
            String attributionTag) {
        TestProvider t;
        synchronized (mLock) {
            t = mTestProviders.get(provider);
        }
        if (t == null) {
            throw new IllegalArgumentException("provider \"" + provider + "\" is not a test"
                    + " provider");
        }
        if (!location.isComplete()) {
            throw new IllegalArgumentException("incomplete location object: " + location);
        }
        Location l = new Location(location);
        l.setMock(true);
        report(provider, l, t.enabled);
    }

    @Override
    public void setTestProviderEnabled(String provider, boolean enabled, String packageName,
            String attributionTag) {
        synchronized (mLock) {
            TestProvider t = mTestProviders.get(provider);
            if (t == null) {
                throw new IllegalArgumentException("provider \"" + provider + "\" is not a test"
                        + " provider");
            }
            t.enabled = enabled;
        }
        log("setTestProviderEnabled " + provider + " " + enabled);
        checkEnabledChanges();
    }

    // --- what Windows has no counterpart for ---------------------------------------------

    @Override
    public boolean isGeocodeAvailable() {
        return false;  // Geocoder.isPresent(): no geocoder
    }

    @Override
    public void reverseGeocode(ReverseGeocodeRequest request, IGeocodeCallback callback) {
        geocodeUnavailable(callback);
    }

    @Override
    public void forwardGeocode(ForwardGeocodeRequest request, IGeocodeCallback callback) {
        geocodeUnavailable(callback);
    }

    private static void geocodeUnavailable(IGeocodeCallback callback) {
        try {
            callback.onError("no geocoder on Gesher");
        } catch (RemoteException e) {
            // gone
        }
    }

    @Override
    public GnssCapabilities getGnssCapabilities() {
        return new GnssCapabilities.Builder().build();
    }

    @Override
    public int getGnssYearOfHardware() {
        return 0;
    }

    @Override
    public String getGnssHardwareModelName() {
        return null;
    }

    @Override
    public int getGnssBatchSize() {
        return 0;
    }

    @Override
    public void registerGnssStatusCallback(android.location.IGnssStatusListener callback,
            String packageName, String attributionTag, String listenerId) {
        enforceLocationPermission(packageName);
        // No GNSS status: never called back.
    }

    @Override
    public void unregisterGnssStatusCallback(android.location.IGnssStatusListener callback) {
    }

    @Override
    public void registerGnssNmeaCallback(android.location.IGnssNmeaListener callback,
            String packageName, String attributionTag, String listenerId) {
        enforceLocationPermission(packageName);
    }

    @Override
    public void unregisterGnssNmeaCallback(android.location.IGnssNmeaListener callback) {
    }

    @Override
    public void requestGeofence(android.location.Geofence geofence, PendingIntent intent,
            String packageName, String attributionTag) {
        enforceLocationPermission(packageName);
        log("requestGeofence by " + packageName + ": geofences are not monitored");
    }

    @Override
    public void removeGeofence(PendingIntent intent) {
    }

    @Override
    public void injectLocation(Location location) {
        if (location != null && location.isComplete()) {
            report(LocationManager.PASSIVE_PROVIDER, location, false);
        }
    }

    @Override
    public void sendExtraCommand(String provider, String command, Bundle extras) {
    }

    @Override
    public android.location.LocationTime getGnssTimeMillis() {
        return null;
    }
}
