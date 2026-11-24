/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.location.Country;
import android.location.ICountryDetector;
import android.location.ICountryListener;
import android.os.IBinder;
import android.os.RemoteException;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.Supplier;

/**
 * {@code country_detector} (android.location.CountryDetector, ICountryDetector), as AOSP's
 * CountryDetectorService over ComprehensiveCountryDetector answers on a device without a
 * mobile network, SIM or a location-based country: the country of the system's locale
 * settings, with source {@link Country#COUNTRY_SOURCE_LOCALE}.
 *
 * <p>On Windows that setting is the user's "Country or region" (Settings, Time and
 * language, Language and region: {@code GetUserDefaultGeoName}), which is a preference and
 * not a detection, and is therefore reported as LOCALE, never as NETWORK, LOCATION or SIM.
 * When Windows has no two-letter region (an M.49 area code, or no natives), the answer is
 * AOSP's own locale source: the default Locale's country. Gesher's location bridge has no
 * geocoder, so there is no location-based country.
 *
 * <p>Listeners receive the current country when they are added (CountryDetectorService
 * .addListener) and again whenever it changes, which Windows signals with
 * WM_SETTINGCHANGE; a dead listener is dropped (linkToDeath).
 */
public final class CountryDetectorService extends ICountryDetector.Default {
    /** What the service needs from Windows. */
    public interface Backend {
        /** Windows' "Country or region" as ISO 3166-1 alpha-2, or null when it has none. */
        String homeRegion();

        /**
         * Calls {@code onChange} (on any thread) when Windows' settings may have changed,
         * until the returned handle is run. The first registration starts nothing that a
         * later one needs again.
         */
        Runnable watch(Runnable onChange);
    }

    private final Backend mWindows;
    private final Supplier<Locale> mLocale;
    private final Object mLock = new Object();
    /** Guarded by mLock. Listener binder -> receiver, in registration order. */
    private final Map<IBinder, Receiver> mReceivers = new LinkedHashMap<>();
    /** Guarded by mLock. The Windows watch while there are listeners. */
    private Runnable mUnwatch;
    /** Guarded by mLock. The country last reported to the listeners. */
    private Country mLastNotified;

    /**
     * @param windows Windows' answers, or null without the natives (then only the
     *     default Locale's country, as AOSP's locale source)
     */
    public CountryDetectorService(Backend windows) {
        this(windows, Locale::getDefault);
    }

    CountryDetectorService(Backend windows, Supplier<Locale> locale) {
        mWindows = windows;
        mLocale = locale;
    }

    private final class Receiver implements IBinder.DeathRecipient {
        final ICountryListener listener;
        final IBinder key;

        Receiver(ICountryListener listener) {
            this.listener = listener;
            this.key = listener.asBinder();
        }

        @Override
        public void binderDied() {
            remove(key);
        }
    }

    /** The current country: Windows' region, else the default Locale's, as LOCALE. */
    Country current() {
        String iso = null;
        Backend windows = mWindows;
        if (windows != null) {
            try {
                iso = alpha2(windows.homeRegion());
            } catch (RuntimeException failed) {
                iso = null;
            }
        }
        if (iso == null) {
            Locale locale = mLocale.get();
            if (locale == null) return null;
            // ComprehensiveCountryDetector.getLocaleCountry: the locale's country as it is,
            // "" included.
            iso = locale.getCountry();
        }
        return new Country(iso, Country.COUNTRY_SOURCE_LOCALE);
    }

    /** An ISO 3166-1 alpha-2 code, or null (M.49 numbers, empty, anything else). */
    static String alpha2(String region) {
        if (region == null || region.length() != 2) return null;
        char a = region.charAt(0);
        char b = region.charAt(1);
        boolean letters = Character.isLetter(a) && a < 0x80 && Character.isLetter(b) && b < 0x80;
        if (!letters) return null;
        String iso = region.toUpperCase(Locale.ROOT);
        // "XX": no ISO 3166-1 code assigned.
        return "XX".equals(iso) ? null : iso;
    }

    @Override
    public Country detectCountry() {
        return current();
    }

    @Override
    public void addCountryListener(ICountryListener listener) throws RemoteException {
        if (listener == null) throw new NullPointerException("listener");
        Receiver receiver = new Receiver(listener);
        synchronized (mLock) {
            try {
                receiver.key.linkToDeath(receiver, 0);
            } catch (RemoteException dead) {
                // CountryDetectorService.addListener: a listener that is already dead is
                // not added (and the caller sees no error).
                return;
            }
            Country country = current();
            if (country != null) {
                try {
                    listener.onCountryDetected(country);
                } catch (RemoteException failed) {
                    receiver.key.unlinkToDeath(receiver, 0);
                    return;
                }
            }
            Receiver previous = mReceivers.put(receiver.key, receiver);
            if (previous != null) previous.key.unlinkToDeath(previous, 0);
            if (mReceivers.size() == 1) {
                mLastNotified = country;
                Backend windows = mWindows;
                if (windows != null && mUnwatch == null) mUnwatch = windows.watch(this::onWindowsChange);
            }
        }
    }

    @Override
    public void removeCountryListener(ICountryListener listener) throws RemoteException {
        if (listener == null) throw new NullPointerException("listener");
        remove(listener.asBinder());
    }

    private void remove(IBinder key) {
        Runnable unwatch = null;
        synchronized (mLock) {
            Receiver receiver = mReceivers.remove(key);
            if (receiver == null) return;
            receiver.key.unlinkToDeath(receiver, 0);
            if (mReceivers.isEmpty()) {
                unwatch = mUnwatch;
                mUnwatch = null;
                mLastNotified = null;
            }
        }
        if (unwatch != null) unwatch.run();
    }

    /** Windows' settings changed: tell the listeners if the country did. */
    void onWindowsChange() {
        List<Receiver> receivers;
        Country country;
        synchronized (mLock) {
            if (mReceivers.isEmpty()) return;
            country = current();
            if (country == null || country.equals(mLastNotified)) return;
            mLastNotified = country;
            receivers = new ArrayList<>(mReceivers.values());
        }
        for (Receiver receiver : receivers) {
            try {
                receiver.listener.onCountryDetected(country);
            } catch (RemoteException failed) {
                // As CountryDetectorService.notifyReceivers: the death notice removes it.
            }
        }
    }

    /** For tests and dumps: the number of registered listeners. */
    int listenerCount() {
        synchronized (mLock) {
            return mReceivers.size();
        }
    }
}
