// © 2016 and later: Unicode, Inc. and others.
// Copyright (C) 1996-2011, International Business Machines Corporation and others.
// License: tools/package/licenses/ICU-CALENDAR-LICENSE.txt
package com.gesher.server;

import android.icu.impl.CalendarAstronomer;

/**
 * The sunrise/sunset portion of ICU's CalendarAstronomer used by AOSP TwilightService.
 * Android's current ICU exposes the solar coordinates but removed rise/set from its
 * boot API. Keep that calculation here, using the same coordinates, constants,
 * iteration limit and five-second tolerance. No replacement twilight model is used.
 * The unmodified upstream implementation is the differential test reference.
 */
final class UiModeSolarCalculator {
    private static final double PI2 = 2 * Math.PI;
    private static final double DEG_RAD = Math.PI / 180;
    private static final double RAD_DEG = 180 / Math.PI;
    private static final long HOUR_MS = 3_600_000L, DAY_MS = 86_400_000L;
    private final CalendarAstronomer mCoordinates = new CalendarAstronomer();
    private final double mLatitude;
    private final long mGmtOffset;

    UiModeSolarCalculator(double longitude, double latitude) {
        double normalizedLongitude = normalize(longitude * DEG_RAD + Math.PI, PI2) - Math.PI;
        mLatitude = normalize(latitude * DEG_RAD + Math.PI, PI2) - Math.PI;
        mGmtOffset = (long) (normalizedLongitude * 24 * HOUR_MS / PI2);
    }
    void setTime(long time) { mCoordinates.setTime(time); }

    long getSunRiseSet(boolean rise) {
        long original = mCoordinates.getTime();
        long noon = ((original + mGmtOffset) / DAY_MS) * DAY_MS - mGmtOffset + 12 * HOUR_MS;
        setTime(noon + (rise ? -6L : 6L) * HOUR_MS);
        CalendarAstronomer.Equatorial position;
        double tanLatitude = Math.tan(mLatitude);
        long difference;
        int count = 0;
        do {
            position = mCoordinates.eclipticToEquatorial(mCoordinates.getSunLongitude(), 0);
            double angle = Math.acos(-tanLatitude * Math.tan(position.declination));
            double sidereal = ((rise ? PI2 - angle : angle) + position.ascension) * 24 / PI2;
            long next = siderealToUniversal(sidereal);
            difference = next - mCoordinates.getTime();
            setTime(next);
        } while (++count < 5 && Math.abs(difference) > 5_000);
        double cosDeclination = Math.cos(position.declination);
        double psi = Math.acos(Math.sin(mLatitude) / cosDeclination);
        double diameterAndRefraction = .533 * DEG_RAD / 2 + 34 / 60.0 * DEG_RAD;
        double correction = Math.asin(Math.sin(diameterAndRefraction) / Math.sin(psi));
        long delta = (long) ((240 * correction * RAD_DEG / cosDeclination) * 1000);
        long result = mCoordinates.getTime() + (rise ? -delta : delta);
        setTime(original);
        return result;
    }
    private long siderealToUniversal(double sidereal) {
        double julianMidnight = Math.floor(mCoordinates.getJulianDay() - 0.5) + 0.5;
        double centuries = (julianMidnight - 2451545.0) / 36525.0;
        double offset = normalize(6.697374558 + 2400.051336 * centuries
                + 0.000025862 * centuries * centuries, 24);
        double local = normalize((sidereal - offset) * 0.9972695663, 24);
        long base = DAY_MS * ((mCoordinates.getTime() + mGmtOffset) / DAY_MS) - mGmtOffset;
        return base + (long) (local * HOUR_MS);
    }
    private static double normalize(double value, double range) {
        return value - range * Math.floor(value / range);
    }
}
