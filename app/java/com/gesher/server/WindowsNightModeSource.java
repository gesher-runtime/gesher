/*
 * Copyright (C) 2017 The Android Open Source Project
 * Copyright (C) 2026 The Gesher Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.gesher.server;

import android.location.Location;
import java.time.ZoneId;
import java.util.Calendar;

/** Real Windows inputs for UiModeManager; no coordinates or theme are invented on failure. */
final class WindowsNightModeSource implements UiModeManagerService.Source, WindowsHost.Listener {
    private volatile Runnable mChanged;
    private volatile ZoneId mZone = ZoneId.systemDefault();

    @Override public long now() { return System.currentTimeMillis(); }
    @Override public ZoneId zone() { return mZone; }
    @Override public UiModeManagerService.Environment read(boolean twilight) {
        // AlarmManager's worker may not yet have invalidated the default-zone
        // cache for this same Windows notification. Observe the host zone here.
        String windowsZone = WindowsHost.timeZoneKeyName();
        if (windowsZone != null) {
            String id = null;
            try { id = android.icu.util.TimeZone.getIDForWindowsID(windowsZone, null); }
            catch (RuntimeException | LinkageError unavailableMapping) { }
            if (id == null) id = WindowsHost.ianaZone(windowsZone);
            if (id != null) {
                try { mZone = ZoneId.of(id); }
                catch (java.time.DateTimeException unavailableZone) { }
            }
        }
        int[] power = WindowsHost.powerStatus();
        return new UiModeManagerService.Environment(WindowsHost.appsUseLightTheme(),
                WindowsHost.sessionDisplayState(), power != null && power.length > 3 && (power[3] == 0 || power[3] == 1)
                        ? power[3] == 1 : null,
                twilight ? twilight() : null);
    }
    @Override public void listen(Runnable changed) {
        mChanged = changed;
        WindowsHost.addListener(this);
    }
    @Override public void close() {
        mChanged = null;
        WindowsHost.removeListener(this);
    }
    @Override public void onWindowsEvents(int events) {
        if ((events & (WindowsHost.EVENT_SETTING_CHANGED | WindowsHost.EVENT_TIME_CHANGED
                | WindowsHost.EVENT_RESUMED | WindowsHost.EVENT_POWER_STATUS
                | WindowsHost.EVENT_DISPLAY_STATE)) != 0) {
            Runnable changed = mChanged;
            if (changed != null) changed.run();
        }
    }
    private UiModeManagerService.Twilight twilight() {
        if (!WindowsLocation.enabled()) return null;
        try {
            Location location = WindowsLocation.fix("fused", false, 3_600_000L, 1_000L);
            return calculateTwilight(location, now(), mZone);
        } catch (WindowsLocation.WindowsException failure) {
            // AOSP retains its last computed night state when twilight has no position.
            return null;
        }
    }

    /** AOSP TwilightService.calculateTwilightState, Android 16 r4 (see docs/ui-mode.md). */
    static UiModeManagerService.Twilight calculateTwilight(Location location, long timeMillis, ZoneId zone) {
        if (location == null) return null;
        UiModeSolarCalculator ca = new UiModeSolarCalculator(location.getLongitude(), location.getLatitude());
        Calendar noon = Calendar.getInstance(java.util.TimeZone.getTimeZone(zone));
        noon.setTimeInMillis(timeMillis);
        noon.set(Calendar.HOUR_OF_DAY, 12);
        noon.set(Calendar.MINUTE, 0);
        noon.set(Calendar.SECOND, 0);
        noon.set(Calendar.MILLISECOND, 0);
        ca.setTime(noon.getTimeInMillis());
        long sunrise = ca.getSunRiseSet(true);
        long sunset = ca.getSunRiseSet(false);
        if (sunset < timeMillis) {
            noon.add(Calendar.DATE, 1);
            ca.setTime(noon.getTimeInMillis());
            sunrise = ca.getSunRiseSet(true);
        } else if (sunrise > timeMillis) {
            noon.add(Calendar.DATE, -1);
            ca.setTime(noon.getTimeInMillis());
            sunset = ca.getSunRiseSet(false);
        }
        // TwilightState.isNight(): the next sunrise follows the previous sunset.
        boolean night = sunrise > sunset;
        return new UiModeManagerService.Twilight(night, night ? sunrise : sunset);
    }
}
