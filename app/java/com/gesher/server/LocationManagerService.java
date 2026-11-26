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
import java.util.Objects;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
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
    private final EditionPolicy mEdition;
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
    // Native location polling may block for seconds. Request lifetime timers must
    // still retire listeners even when no fix arrives from that worker.
    private final ScheduledExecutorService mExpirationExecutor = newExpirationExecutor();

    private static ScheduledExecutorService newExpirationExecutor() {
        ScheduledThreadPoolExecutor executor = new ScheduledThreadPoolExecutor(1, r -> {
            Thread t = new Thread(r, "location-expiration");
            t.setDaemon(true);
            return t;
        });
        executor.setRemoveOnCancelPolicy(true);
        return executor;
    }

    private ScheduledFuture<?> mPoll;
    private long mPollInterval;
    /** The enabled state each provider last reported to the registrations. */
    private final Map<String, Boolean> mReportedEnabled = new HashMap<>();
    /** isLocationEnabled's last reported answer (MODE_CHANGED, location_mode); null: none yet. */
    private Boolean mReportedLocationEnabled;
    /** Serializes location-mode checks, so an older answer is never reported last. */
    private final Object mModeLock = new Object();

    public LocationManagerService(PackageManagerService pm, ActivityManagerService am) {
        this(pm, am, EditionPolicy.LEGACY);
    }

    LocationManagerService(PackageManagerService pm, ActivityManagerService am, EditionPolicy edition) {
        mPm = pm;
        mAm = am;
        mEdition = Objects.requireNonNull(edition);
        installSettingsState();
        if (!permitsHostLocation()) return;
        // Windows' answer may take a moment: asked now, off the binder threads.
        mExecutor.execute(() -> {
            WindowsLocation.refreshAccess();
            log("Windows location: " + WindowsLocation.describe());
            checkEnabledChanges();
        });
    }

    private boolean permitsHostLocation() {
        return mEdition.permits(EditionPolicy.Capability.HOST_LOCATION);
    }

    /**
     * Settings.Secure location_mode answers this service's state, as Android's location
     * service keeps it (react-native-wifi-reborn reads it before every Wi-Fi scan).
     */
    void installSettingsState() {
        SettingsProvider.setLocationState(() -> {
            boolean enabled = isLocationEnabled();
            Boolean reported;
            synchronized (mLock) {
                reported = mReportedLocationEnabled;
            }
            // A change a reader saw first: observers and receivers learn of it too.
            if (reported != null && reported != enabled) {
                try {
                    mExecutor.execute(this::checkEnabledChanges);
                } catch (java.util.concurrent.RejectedExecutionException shutDown) {
                }
            }
            return enabled;
        });
    }

    static void log(String message) {
        SystemServer.log(TAG + ": " + message);
    }

    private void maybePromptWindowsLocation(String packageName) {
        if (WindowsLocation.enabled() || isIpFallbackEnabled(null)) return;
        int uid = Binder.getCallingUid();
        if (uid < android.os.Process.FIRST_APPLICATION_UID || mPm == null) return;
        String target = null;
        if (packageName != null && grantedLocationLevel(packageName, false) != PERMISSION_NONE) {
            target = packageName;
        } else {
            String[] packages = mPm.getPackagesForUid(uid);
            if (packages != null) {
                for (String p : packages) {
                    if (grantedLocationLevel(p, false) != PERMISSION_NONE) {
                        target = p;
                        break;
                    }
                }
            }
        }
        if (target != null) WindowsLocation.maybePromptEnable(target, target);
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
        return permitsHostLocation() && (WindowsLocation.enabled() || isIpFallbackEnabled(provider));
    }

    /** Location is on when Windows gives it to this program, or a test provider is on. */
    boolean isLocationEnabled() {
        synchronized (mLock) {
            for (TestProvider t : mTestProviders.values()) {
                if (t.enabled) return true;
            }
        }
        return permitsHostLocation() && (WindowsLocation.enabled() || isIpFallbackEnabled(null));
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
        boolean enabled = provider != null && exists(provider) && isEnabled(provider);
        if (!enabled && provider != null && exists(provider) && !isTest(provider)) {
            maybePromptWindowsLocation(null);
        }
        return enabled;
    }

    @Override
    public boolean isLocationEnabledForUser(int userId) {
        boolean enabled = isLocationEnabled();
        if (!enabled) maybePromptWindowsLocation(null);
        return enabled;
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
        CallerIdentity callers = mAm != null ? mAm.callers() : new CallerIdentity(mPm, null);
        if (!callers.isSystem()) callers.requirePackage(packageName);
        return grantedLocationLevel(packageName, callers.isSystem());
    }

    private int grantedLocationLevel(String packageName, boolean system) {
        ApkPackage p = mPm.get(packageName);
        if (p == null) {
            return system && (packageName == null || "android".equals(packageName))
                    ? PERMISSION_FINE : PERMISSION_NONE;
        }
        if (mPm.permissions().checkPermission(packageName, Manifest.permission.ACCESS_FINE_LOCATION)
                == android.content.pm.PackageManager.PERMISSION_GRANTED) return PERMISSION_FINE;
        if (mPm.permissions().checkPermission(packageName, Manifest.permission.ACCESS_COARSE_LOCATION)
                == android.content.pm.PackageManager.PERMISSION_GRANTED) {
            return PERMISSION_COARSE;
        }
        return PERMISSION_NONE;
    }

    private void enforceMockPermission(String packageName) {
        if (!GmsSignatureSpoofing.isEnabled()) return;
        CallerIdentity callers = mAm != null ? mAm.callers() : new CallerIdentity(mPm, null);
        if (callers.isSystem()) return;
        String actual = callers.requirePackage(packageName);
        if (mPm.permissions().checkPermission(actual, "android.permission.ACCESS_MOCK_LOCATION")
                != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Mock location requires explicit system authorization");
        }
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
        if (!isEnabled(provider)) {
            if (!isTest(provider)) maybePromptWindowsLocation(packageName);
            return null;
        }
        Location last;
        synchronized (mLock) {
            last = mLast.get(provider);
        }
        if (last == null && permitsHostLocation() && !isTest(provider)
                && !LocationManager.PASSIVE_PROVIDER.equals(provider)) {
            if (WindowsLocation.enabled()) {
                // Windows keeps a last position too: a cached one, if any, quickly.
                try {
                    last = WindowsLocation.fix(provider, false, 24L * 3600_000, 1000);
                    report(provider, last, false);
                } catch (WindowsLocation.WindowsException e) {
                    log("no last location from Windows: " + e.getMessage());
                }
            }
            if (last == null && isIpFallbackEnabled(provider)) {
                last = getCachedIpLocation(provider);
                if (last != null) {
                    report(provider, last, false);
                }
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
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(callback, "callback");
        IBinder client = Objects.requireNonNull(callback.asBinder(), "callback binder");
        ICancellationSignal transport = CancellationSignal.createTransport();
        CancellationSignal cancel = CancellationSignal.fromTransport(transport);
        Registration r = new Registration(provider, request, packageName, level);
        r.current = callback;
        r.clientBinder = client;
        r.maxUpdates = 1;
        r.expiresAt = Math.min(r.expiresAt, SystemClock.elapsedRealtime() + 30_000);
        cancel.setOnCancelListener(() -> remove(r));
        if (!add(r)) return transport;
        if (!isEnabled(provider)) {
            r.providerEnabledChanged(false);
            return transport;
        }
        Location last;
        synchronized (mLock) {
            last = mLast.get(provider);
        }
        if (last != null && ageMillis(last) <= CURRENT_MAX_AGE_MS) {
            r.deliver(last);
        }
        return transport;
    }

    private static void deliverCurrent(ILocationCallback callback, Location location) {
        try {
            callback.onLocation(location);
        } catch (RemoteException | RuntimeException e) {
            log("current location delivery failed: " + e);
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
        final boolean systemCaller;
        ILocationListener listener;
        PendingIntent pendingIntent;
        ILocationCallback current;
        IBinder clientBinder;
        /** Retirement may precede publication when linkToDeath reports death inline. */
        volatile boolean retired;
        /** Protected by mLock; unlinking itself happens outside the registry lock. */
        boolean deathLinked;
        ScheduledFuture<?> expirationTask;
        // Delivery admission and its count/last fix are protected by mLock.
        int maxUpdates;
        int updates;
        long expiresAt = Long.MAX_VALUE;
        Location lastDelivered;
        Boolean lastProviderEnabled;

        Registration(String provider, LocationRequest request, String packageName, int level) {
            this.provider = provider;
            this.request = request;
            this.packageName = packageName;
            this.level = level;
            this.systemCaller = (mAm != null ? mAm.callers()
                    : new CallerIdentity(mPm, null)).isSystem();
            this.maxUpdates = request.getMaxUpdates();
            expiresAt = request.getExpirationRealtimeMs(SystemClock.elapsedRealtime());
        }

        boolean replaces(Registration other) {
            return provider.equals(other.provider) && clientBinder == other.clientBinder
                    && ((listener != null && other.listener != null)
                        || (pendingIntent != null && other.pendingIntent != null));
        }

        @Override
        public void binderDied() {
            remove(this);
        }

        /** Reserve a delivery before calling client code, including its terminal claim. */
        void deliver(Location location) {
            int granted = grantedLocationLevel(packageName, systemCaller);
            Location l = null;
            boolean finished = false;
            synchronized (mLock) {
                if (retired) return;
                if (granted == PERMISSION_NONE || SystemClock.elapsedRealtime() >= expiresAt) {
                    // An expired current request still owes one null result, even
                    // if a late fix gets here before the timer executes.
                    retireLocked(this);
                    finished = true;
                } else {
                    if (lastDelivered != null) {
                        long minInterval = request.getMinUpdateIntervalMillis();
                        long since = (location.getElapsedRealtimeNanos()
                                - lastDelivered.getElapsedRealtimeNanos()) / 1_000_000L;
                        if (since < minInterval - Math.min(minInterval / 10, 30_000)) return;
                        if (lastDelivered.distanceTo(location) < request.getMinUpdateDistanceMeters()) {
                            return;
                        }
                    }
                    l = forPermission(location, Math.min(level, granted));
                    if (!provider.equals(l.getProvider())) l.setProvider(provider);
                    lastDelivered = new Location(location);
                    updates++;
                    if (updates >= maxUpdates) {
                        retireLocked(this);
                        finished = true;
                    }
                }
            }
            if (finished) {
                unlink(this);
                updatePolling();
            }
            try {
                if (current != null) {
                    current.onLocation(l);
                } else if (l == null) {
                    return;
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
                remove(this);
            }
        }

        void providerEnabledChanged(boolean enabled) {
            if (current != null) {
                if (!enabled && remove(this)) deliverCurrent(current, null);
                return;
            }
            synchronized (mLock) {
                if (retired || (lastProviderEnabled != null && lastProviderEnabled == enabled)) {
                    return;
                }
                // Registration and the asynchronous provider check may observe
                // the same initial disabled state. Claim each change once.
                lastProviderEnabled = enabled;
            }
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

    private boolean add(Registration r) {
        boolean linksDeath = r.listener != null || r.current != null;
        if (linksDeath) {
            try {
                r.clientBinder.linkToDeath(r, 0);
            } catch (RemoteException e) {
                remove(r);
                return false;
            }
        }
        List<Registration> replaced = new ArrayList<>();
        boolean published;
        synchronized (mLock) {
            r.deathLinked = linksDeath;
            published = !r.retired;
            if (published) {
                for (Iterator<Registration> it = mRegistrations.iterator(); it.hasNext();) {
                    Registration previous = it.next();
                    if (r.replaces(previous)) {
                        previous.retired = true;
                        it.remove();
                        replaced.add(previous);
                    }
                }
                mRegistrations.add(r);
                scheduleExpirationLocked(r);
            }
        }
        if (!published) unlink(r);
        for (Registration previous : replaced) unlink(previous);
        updatePolling();
        return published;
    }

    /** Release record-owned timer/death registrations outside the registry lock. */
    private void unlink(Registration r) {
        boolean linked;
        ScheduledFuture<?> expiration;
        synchronized (mLock) {
            linked = r.deathLinked;
            r.deathLinked = false;
            expiration = r.expirationTask;
            r.expirationTask = null;
        }
        if (expiration != null) expiration.cancel(false);
        if (linked) {
            try {
                r.clientBinder.unlinkToDeath(r, 0);
            } catch (RuntimeException e) {
                // Already dead or no longer linked.
            }
        }
    }

    private boolean remove(Registration r) {
        boolean removed;
        synchronized (mLock) {
            removed = retireLocked(r);
        }
        unlink(r);
        if (removed) {
            updatePolling();
        }
        return removed;
    }

    /** mLock serializes terminal success, expiry, replacement, cancellation and death. */
    private boolean retireLocked(Registration r) {
        r.retired = true;
        return mRegistrations.remove(r);
    }

    private void scheduleExpirationLocked(Registration r) {
        if (r.expiresAt == Long.MAX_VALUE) return;
        long delay = Math.max(0, r.expiresAt - SystemClock.elapsedRealtime());
        r.expirationTask = mExpirationExecutor.schedule(() -> expire(r), delay,
                TimeUnit.MILLISECONDS);
    }

    private void expire(Registration r) {
        synchronized (mLock) {
            if (r.retired) return;
            // A scheduler wakeup may precede the elapsed-realtime deadline.
            if (SystemClock.elapsedRealtime() < r.expiresAt) {
                scheduleExpirationLocked(r);
                return;
            }
            retireLocked(r);
        }
        unlink(r);
        updatePolling();
        if (r.current != null) deliverCurrent(r.current, null);
    }

    @Override
    public void registerLocationListener(String provider, LocationRequest request,
            ILocationListener listener, String packageName, String attributionTag,
            String listenerId) {
        int level = enforceLocationPermission(packageName);
        checkProvider(provider);
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(listener, "listener");
        IBinder client = Objects.requireNonNull(listener.asBinder(), "listener binder");
        Registration r = new Registration(provider, request, packageName, level);
        r.listener = listener;
        r.clientBinder = client;
        if (!add(r)) return;
        log("registerLocationListener " + provider + " for " + packageName + ": interval "
                + request.getIntervalMillis() + " ms");
        if (!isEnabled(provider)) r.providerEnabledChanged(false);
    }

    @Override
    public void unregisterLocationListener(ILocationListener listener) {
        IBinder client = listener == null ? null : listener.asBinder();
        List<Registration> gone = new ArrayList<>();
        synchronized (mLock) {
            for (Iterator<Registration> it = mRegistrations.iterator(); it.hasNext();) {
                Registration r = it.next();
                if (r.listener != null && r.clientBinder == client) {
                    r.retired = true;
                    it.remove();
                    gone.add(r);
                }
            }
        }
        for (Registration r : gone) {
            unlink(r);
            log("unregisterLocationListener " + r.provider + " for " + r.packageName);
        }
        if (!gone.isEmpty()) updatePolling();
    }

    @Override
    public void registerLocationPendingIntent(String provider, LocationRequest request,
            PendingIntent pendingIntent, String packageName, String attributionTag) {
        int level = enforceLocationPermission(packageName);
        checkProvider(provider);
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(pendingIntent, "pendingIntent");
        IBinder client = Objects.requireNonNull(pendingIntent.getTarget().asBinder(), "pending intent binder");
        Registration r = new Registration(provider, request, packageName, level);
        r.pendingIntent = pendingIntent;
        r.clientBinder = client;
        add(r);
        log("registerLocationPendingIntent " + provider + " for " + packageName);
        if (!isEnabled(provider)) r.providerEnabledChanged(false);
    }

    @Override
    public void unregisterLocationPendingIntent(PendingIntent pendingIntent) {
        IBinder client = pendingIntent == null ? null : pendingIntent.getTarget().asBinder();
        List<Registration> gone = new ArrayList<>();
        synchronized (mLock) {
            for (Iterator<Registration> it = mRegistrations.iterator(); it.hasNext();) {
                Registration r = it.next();
                if (r.pendingIntent != null && r.clientBinder == client) {
                    r.retired = true;
                    it.remove();
                    gone.add(r);
                }
            }
        }
        for (Registration r : gone) {
            unlink(r);
            log("unregisterLocationPendingIntent " + r.provider + " for " + r.packageName);
        }
        if (!gone.isEmpty()) updatePolling();
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
        for (Registration r : targets) r.deliver(location);
    }

    /** Polls Windows for the registrations on its providers, at their shortest interval. */
    private void updatePolling() {
        if (!permitsHostLocation()) return;
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
        if (!permitsHostLocation()) return;
        checkEnabledChanges();
        boolean windowsEnabled = WindowsLocation.enabled();
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
        Location fix = null;
        if (windowsEnabled) {
            try {
                fix = WindowsLocation.fix(providers.get(0), high, mPollInterval,
                        Math.min(Math.max(mPollInterval, 5000), 30_000));
            } catch (WindowsLocation.WindowsException e) {
                log("Windows gave no position: " + e.getMessage() + ", trying IP geolocation fallback");
            }
        }
        if (fix == null && isIpFallbackEnabled(providers.get(0))) {
            fix = getIpLocation(providers.get(0));
        }
        if (fix == null) return;
        for (String p : providers) report(p, fix, true);
    }

    // --- IP-based geolocation fallback ---------------------------------------------------

    private static final String IP_GEO_URL = "http://ip-api.com/json/?fields=status,message,country,city,lat,lon";
    private static final long IP_CACHE_MS = 600_000;  // 10 minutes cache
    private static final Object sIpLock = new Object();
    private static Location sCachedIpLocation;
    private static long sCachedIpTime;
    private static volatile boolean sIpFallbackAvailable = true;

    private boolean isIpFallbackEnabled(String provider) {
        if (GmsSignatureSpoofing.isEnabled()
                && !"1".equals(System.getenv("GESHER_IP_GEOLOCATION"))) return false;
        if (provider != null && LocationManager.GPS_PROVIDER.equals(provider)) {
            return false;
        }
        return sIpFallbackAvailable;
    }

    private static Location getCachedIpLocation(String provider) {
        synchronized (sIpLock) {
            if (sCachedIpLocation == null) return null;
            Location copy = new Location(sCachedIpLocation);
            copy.setProvider(provider);
            copy.setTime(System.currentTimeMillis());
            copy.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
            return copy;
        }
    }

    private Location getIpLocation(String provider) {
        long now = SystemClock.elapsedRealtime();
        synchronized (sIpLock) {
            if (sCachedIpLocation != null && (now - sCachedIpTime) < IP_CACHE_MS) {
                Location copy = new Location(sCachedIpLocation);
                copy.setProvider(provider);
                copy.setTime(System.currentTimeMillis());
                copy.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
                return copy;
            }
        }

        try {
            java.net.URL url = new java.net.URL(IP_GEO_URL);
            java.net.HttpURLConnection conn = (java.net.HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(3000);
            conn.setReadTimeout(3000);
            conn.setRequestMethod("GET");
            conn.setRequestProperty("User-Agent", "Gesher/1.0");

            if (conn.getResponseCode() == 200) {
                java.io.BufferedReader reader = new java.io.BufferedReader(
                        new java.io.InputStreamReader(conn.getInputStream(),
                                java.nio.charset.StandardCharsets.UTF_8));
                StringBuilder sb = new StringBuilder();
                String line;
                while ((line = reader.readLine()) != null) {
                    sb.append(line);
                }
                reader.close();

                String json = sb.toString();
                double lat = parseJsonDouble(json, "\"lat\":");
                double lon = parseJsonDouble(json, "\"lon\":");
                if (!Double.isNaN(lat) && !Double.isNaN(lon)) {
                    Location loc = new Location(provider);
                    loc.setLatitude(lat);
                    loc.setLongitude(lon);
                    loc.setAccuracy(5000.0f);
                    loc.setTime(System.currentTimeMillis());
                    loc.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
                    Bundle extras = new Bundle();
                    extras.putString("windowsPositionSource", "ip-address");
                    loc.setExtras(extras);

                    synchronized (sIpLock) {
                        sCachedIpLocation = loc;
                        sCachedIpTime = now;
                    }
                    log("IP geolocation resolved fallback location: " + lat + ", " + lon);
                    return loc;
                }
            }
        } catch (Exception e) {
            log("IP geolocation fallback request error: " + e.getMessage());
        }

        synchronized (sIpLock) {
            if (sCachedIpLocation != null) {
                Location copy = new Location(sCachedIpLocation);
                copy.setProvider(provider);
                copy.setTime(System.currentTimeMillis());
                copy.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
                return copy;
            }
        }
        return null;
    }

    private static double parseJsonDouble(String json, String key) {
        int idx = json.indexOf(key);
        if (idx < 0) return Double.NaN;
        idx += key.length();
        while (idx < json.length() && Character.isWhitespace(json.charAt(idx))) idx++;
        int start = idx;
        if (start < json.length() && (json.charAt(start) == '-' || json.charAt(start) == '+')) {
            idx++;
        }
        while (idx < json.length() && (Character.isDigit(json.charAt(idx)) || json.charAt(idx) == '.')) {
            idx++;
        }
        try {
            return Double.parseDouble(json.substring(start, idx));
        } catch (Exception e) {
            return Double.NaN;
        }
    }

    /**
     * Tells everyone about location turned on or off (location_mode's observers, then
     * MODE_CHANGED, as Android does), then registrations and receivers about providers
     * turned on or off, since the last check. Windows' state is seen when a request polls
     * it, a test provider changes or location_mode is read (WindowsLocation re-asks Windows
     * at most every 10 s); there is no Windows notification to wait on.
     */
    private void checkEnabledChanges() {
        checkLocationModeChange();
        checkProviderChanges();
    }

    private void checkLocationModeChange() {
        synchronized (mModeLock) {
            boolean location = isLocationEnabled();
            Boolean before;
            synchronized (mLock) {
                before = mReportedLocationEnabled;
                mReportedLocationEnabled = location;
            }
            // A first observation initializes the state; it is not a change.
            if (before == null || before == location) return;
            log("location " + (location ? "enabled" : "disabled"));
            SettingsProvider.locationStateChanged();
            Intent changed = new Intent(LocationManager.MODE_CHANGED_ACTION);
            changed.putExtra(LocationManager.EXTRA_LOCATION_ENABLED, location);
            changed.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY | Intent.FLAG_RECEIVER_FOREGROUND);
            try {
                mAm.sendBroadcastFor("android", changed, null);
            } catch (RuntimeException e) {
                log("MODE_CHANGED broadcast failed: " + e);
            }
        }
    }

    /** The provider part of checkEnabledChanges. */
    private void checkProviderChanges() {
        List<String> providers = getAllProviders();
        for (String p : providers) {
            boolean enabled = isEnabled(p);
            Boolean before;
            synchronized (mLock) {
                before = mReportedEnabled.put(p, enabled);
            }
            // A first disabled observation must also complete requests admitted
            // before the asynchronous initial provider check.
            if (before == null ? enabled : before == enabled) continue;
            log("provider " + p + (enabled ? " enabled" : " disabled"));
            List<Registration> targets = new ArrayList<>();
            synchronized (mLock) {
                for (Registration r : mRegistrations) {
                    if (r.provider.equals(p)) targets.add(r);
                }
            }
            for (Registration r : targets) r.providerEnabledChanged(enabled);
            // A first observation initializes provider state; it is not a
            // provider transition to broadcast to unrelated receivers.
            if (before == null) continue;
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
        enforceMockPermission(packageName);
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
        enforceMockPermission(packageName);
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
            for (Registration r : gone) {
                if (remove(r) && r.current != null) deliverCurrent(r.current, null);
            }
        }
        updatePolling();
        checkEnabledChanges();
    }

    @Override
    public void setTestProviderLocation(String provider, Location location, String packageName,
            String attributionTag) {
        enforceMockPermission(packageName);
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
        enforceMockPermission(packageName);
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
        if (GmsSignatureSpoofing.isEnabled()) {
            CallerIdentity callers = mAm != null ? mAm.callers() : new CallerIdentity(mPm, null);
            if (!callers.isSystem()) throw new SecurityException("Only the host can inject location");
        }
        if (permitsHostLocation() && location != null && location.isComplete()) {
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
