/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.time.LocalDate;
import java.time.ZoneOffset;

/** Differential authority: unchanged ICU implementation used by AOSP's twilight service. */
public final class SolarComparisonHost {
    public static void main(String[] args) {
        int checks = 0;
        for (int year : new int[]{1970, 2000, 2024, 2026, 2038}) {
            for (int month = 1; month <= 12; month++) {
                long noon = LocalDate.of(year, month, 17).atTime(12, 0).toInstant(ZoneOffset.UTC).toEpochMilli();
                for (double latitude : new double[]{-90, -80, -66, -45, 0, 32, 51.5, 66, 80, 90}) {
                    for (double longitude : new double[]{-179, -122.4, 0, 35.2, 139.7, 179}) {
                        var expected = new com.ibm.icu.impl.CalendarAstronomer(longitude, latitude);
                        var actual = new UiModeSolarCalculator(longitude, latitude);
                        expected.setTime(noon); actual.setTime(noon);
                        for (boolean rise : new boolean[]{true, false}) {
                            long reference = expected.getSunRiseSet(rise), result = actual.getSunRiseSet(rise);
                            if (reference != result) throw new AssertionError("AOSP solar difference: "
                                    + year + "/" + month + " " + latitude + "," + longitude + " rise=" + rise
                                    + " expected=" + reference + " actual=" + result);
                            checks++;
                        }
                    }
                }
            }
        }
        System.out.println("AOSP solar differential all passed: " + checks + " exact millisecond results");
    }
}
