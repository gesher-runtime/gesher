/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * CountryDetectorService's Windows backend: the user's "Country or region"
 * (WindowsHost.homeRegion: GetUserDefaultGeoName, else GetUserGeoID/GetGeoInfoW GEO_ISO2)
 * and WM_SETTINGCHANGE (WindowsHost.EVENT_SETTING_CHANGED), which Windows broadcasts when
 * the user changes it.
 */
final class WindowsCountry implements CountryDetectorService.Backend {
    private WindowsCountry() {
    }

    /** Null when this runtime has no Windows natives. */
    static WindowsCountry create() {
        return WindowsHost.available() ? new WindowsCountry() : null;
    }

    @Override
    public String homeRegion() {
        return WindowsHost.homeRegion();
    }

    @Override
    public Runnable watch(Runnable onChange) {
        WindowsHost.Listener listener = events -> {
            if ((events & WindowsHost.EVENT_SETTING_CHANGED) != 0) onChange.run();
        };
        WindowsHost.addListener(listener);
        return () -> WindowsHost.removeListener(listener);
    }
}
