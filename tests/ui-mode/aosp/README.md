# AOSP solar calculation reference

`com/ibm/icu/impl/CalendarAstronomer.java` is an unchanged copy of the
[legacy ICU implementation built as `icu4j_calendar_astronomer`](https://android.googlesource.com/platform/external/icu/+/android-14.0.0_r1/icu4j/main/classes/core/src/com/ibm/icu/impl/CalendarAstronomer.java).
Its SHA-256 is `0acaa33b5d5079333bd5aa2b2e9a1b622a3e5dcdda50ca53eab345fd58c5201d`.
The upstream Unicode/ICU license is retained in
`tools/package/licenses/ICU-CALENDAR-LICENSE.txt`.

Android 16 r4's pinned [TwilightService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/twilight/TwilightService.java)
still uses this separate `com.ibm.icu` sunrise/sunset API. Gesher's Android 16
boot ICU exposes the solar coordinates but no longer exposes its rise/set API.
`UiModeSolarCalculator` retains that part of the original calculation and delegates
coordinates to the boot ICU. `SolarComparisonHost` compares 7,200 sunrise/sunset
outputs, including polar locations, dates from 1970 through 2038, both hemispheres
and longitudes across the date line, against this independent unchanged source.
All comparisons require identical millisecond results. This reference is compiled
only in the host tests; it is not packaged in the application or platform.
