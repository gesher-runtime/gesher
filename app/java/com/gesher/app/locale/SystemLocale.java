/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.locale;

import android.icu.text.DecimalFormatSymbols;
import android.icu.text.NumberingSystem;
import android.icu.util.Calendar;
import android.icu.util.LocaleData;
import android.icu.util.ULocale;
import android.os.LocaleList;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * The device's locales, from Windows (docs/locale.md; docs/decisions/0044-rtl-scripts-and-locale.md,
 * 0049-locale-per-app-and-formats.md). What a device's Settings app keeps in
 * {@code persist.sys.locale} and the system server puts in every process's
 * Configuration, Gesher takes from the user's Windows settings:
 *
 * <ul>
 * <li>the locale list is Windows' preferred UI (display) languages, in order
 *     ({@code GetUserPreferredUILanguages});
 * <li>the first locale takes the user's regional format ({@code GetUserDefaultLocaleName})
 *     when it is the same language (an en-US display with en-GB formats is en-GB);
 * <li>the regional settings that differ from that locale's own defaults become Unicode
 *     extensions on it, as Android 14+ keeps its regional preferences: the calendar
 *     ({@code -u-ca-}, {@code LOCALE_ICALENDARTYPE}), the first day of the week
 *     ({@code -u-fw-}, {@code LOCALE_IFIRSTDAYOFWEEK}), the hour cycle
 *     ({@code -u-hc-h23}/{@code h12}, the time format), the temperature unit
 *     ({@code -u-mu-celsius}/{@code fahrenhe}, from the measurement system
 *     {@code LOCALE_IMEASURE}), the digits ({@code -u-nu-}, {@code LOCALE_IDIGITSUBSTITUTION}).
 * </ul>
 *
 * <p>Windows' custom decimal and grouping symbols and date patterns have no Android
 * equivalent (an app formats with its locale's own); they are listed by
 * {@link #customFormats} for the logs, not applied.
 *
 * <p>{@code $GESHER_LOCALES} (comma-separated language tags, e.g. {@code he-IL,en-US})
 * replaces the list as it is. With neither, the list is en-US. The layout direction
 * follows the first locale (Configuration.setLayoutDirection): right to left for Hebrew,
 * Arabic, Persian, Urdu...
 *
 * <p>The list is computed once per process and again on {@link #refresh} (the system
 * server, when Windows' settings change: WM_SETTINGCHANGE).
 */
public final class SystemLocale {
    /** The environment variable that sets the locale list (language tags, ',' between). */
    public static final String ENV = "GESHER_LOCALES";

    // GetLocaleInfoEx's LCTYPEs.
    private static final int LOCALE_IMEASURE = 0x0D;
    private static final int LOCALE_SDECIMAL = 0x0E;
    private static final int LOCALE_STHOUSAND = 0x0F;
    private static final int LOCALE_SSHORTDATE = 0x1F;
    private static final int LOCALE_SLONGDATE = 0x20;
    private static final int LOCALE_STIMEFORMAT = 0x1003;
    private static final int LOCALE_ICALENDARTYPE = 0x1009;
    private static final int LOCALE_IFIRSTDAYOFWEEK = 0x100C;
    private static final int LOCALE_IDIGITSUBSTITUTION = 0x1014;
    private static final int LOCALE_SSHORTTIME = 0x79;
    /** The regional format's own value, without the user's overrides. */
    private static final int LOCALE_NOUSEROVERRIDE = 0x80000000;

    private static final String[] DAYS = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

    private static LocaleList sLocales;
    private static String sSource;
    private static Boolean sIs24Hour;

    private SystemLocale() {
    }

    /** The device's locale list (computed once per process, again on {@link #refresh}). */
    public static synchronized LocaleList locales() {
        if (sLocales == null) {
            compute();
        }
        return sLocales;
    }

    /** Where {@link #locales} came from, for logs. */
    public static synchronized String describe() {
        locales();
        return sLocales.toLanguageTags() + " (" + sSource + ")";
    }

    /**
     * Whether times are shown with 24 hours, as Windows' time format says (Settings'
     * {@code TIME_12_24}); without Windows' answer, as the first locale's.
     */
    public static synchronized boolean is24Hour() {
        if (sIs24Hour == null) {
            String format = windowsInfo(LOCALE_SSHORTTIME);
            if (format == null) format = windowsInfo(LOCALE_STIMEFORMAT);
            sIs24Hour = format != null ? hasHour24(format) : localeIs24Hour(locales().get(0));
        }
        return sIs24Hour;
    }

    /** Sets the process's default locales (LocaleList, Locale) to the device's. */
    public static void applyDefault() {
        LocaleList.setDefault(locales());
    }

    /**
     * Reads Windows' settings again (they changed: WM_SETTINGCHANGE). Returns what changed,
     * for the log ({@code "locales en-US → en-US-u-fw-mon", "24-hour false → true"}), or null
     * when nothing did.
     */
    public static synchronized String refresh() {
        LocaleList oldLocales = locales();
        boolean old24 = is24Hour();
        sLocales = null;
        sSource = null;
        sIs24Hour = null;
        LocaleList now = locales();
        boolean now24 = is24Hour();
        List<String> changes = new ArrayList<>();
        if (!now.equals(oldLocales)) {
            changes.add("locales " + oldLocales.toLanguageTags() + " → " + now.toLanguageTags());
        }
        if (now24 != old24) changes.add("24-hour " + old24 + " → " + now24);
        return changes.isEmpty() ? null : String.join(", ", changes);
    }

    /**
     * Windows' custom formats that Android has no setting for, so not carried over: the
     * decimal and grouping symbols when they are not the first locale's, the short and long
     * date patterns and the time format when the user changed them from the regional
     * format's own. Empty when there are none (or with $GESHER_LOCALES).
     */
    public static synchronized List<String> customFormats() {
        List<String> out = new ArrayList<>();
        if (sSource != null && sSource.startsWith("$")) return out;
        Locale first = locales().get(0);
        DecimalFormatSymbols symbols = DecimalFormatSymbols.getInstance(ULocale.forLocale(first));
        String decimal = windowsInfo(LOCALE_SDECIMAL);
        if (decimal != null && !decimal.equals(String.valueOf(symbols.getDecimalSeparator()))) {
            out.add("decimal symbol '" + decimal + "' (" + first.toLanguageTag() + ": '"
                    + symbols.getDecimalSeparator() + "')");
        }
        String grouping = windowsInfo(LOCALE_STHOUSAND);
        if (grouping != null
                && !grouping.equals(String.valueOf(symbols.getGroupingSeparator()))) {
            out.add("grouping symbol '" + grouping + "' (" + first.toLanguageTag() + ": '"
                    + symbols.getGroupingSeparator() + "')");
        }
        for (int[] t : new int[][] {{LOCALE_SSHORTDATE}, {LOCALE_SLONGDATE},
                {LOCALE_SSHORTTIME}, {LOCALE_STIMEFORMAT}}) {
            String user = windowsInfo(t[0]);
            String own = windowsInfo(t[0] | LOCALE_NOUSEROVERRIDE);
            if (user != null && own != null && !user.equals(own)) {
                String name = t[0] == LOCALE_SSHORTDATE ? "short date"
                        : t[0] == LOCALE_SLONGDATE ? "long date"
                        : t[0] == LOCALE_SSHORTTIME ? "short time" : "time";
                out.add(name + " '" + user + "' (the regional format's: '" + own + "')");
            }
        }
        return out;
    }

    private static void compute() {
        String env = System.getenv(ENV);
        if (env != null && !env.trim().isEmpty()) {
            LocaleList list = LocaleList.forLanguageTags(env.trim().replace(" ", ""));
            if (!list.isEmpty()) {
                sLocales = list;
                sSource = "$" + ENV;
                return;
            }
        }
        String[] ui = null;
        try {
            ui = nativePreferredUiLanguages();
        } catch (LinkageError e) {
            sSource = "no natives: " + e;
        }
        if (ui == null || ui.length == 0) {
            sLocales = LocaleList.forLanguageTags("en-US");
            if (sSource == null) sSource = "Windows gave no UI languages";
            return;
        }
        List<Locale> locales = new ArrayList<>();
        for (String tag : ui) {
            Locale l = Locale.forLanguageTag(tag);
            if (!l.getLanguage().isEmpty()) locales.add(l);
        }
        if (locales.isEmpty()) {
            sLocales = LocaleList.forLanguageTags("en-US");
            sSource = "Windows' UI languages " + String.join(",", ui) + " are not language tags";
            return;
        }
        StringBuilder how = new StringBuilder("Windows' display languages");
        Locale first = locales.get(0);
        String formatName = null;
        try {
            formatName = nativeUserLocaleName();
        } catch (LinkageError e) {
            // No regional format: the display language's own.
        }
        if (formatName != null) {
            Locale format = Locale.forLanguageTag(formatName);
            if (format.getLanguage().equals(first.getLanguage())
                    && (format.getScript().isEmpty() || first.getScript().isEmpty()
                            || format.getScript().equals(first.getScript()))
                    && !format.getCountry().isEmpty()
                    && !format.getCountry().equals(first.getCountry())) {
                first = new Locale.Builder().setLocale(first).setRegion(format.getCountry())
                        .build();
                how.append(", region from the regional format ").append(formatName);
            }
        }
        Locale.Builder b = new Locale.Builder().setLocale(first);
        List<String> prefs = new ArrayList<>();
        String[][] keywords = {
            {"ca", calendar(first)},
            {"fw", firstDayOfWeek(first)},
            {"hc", hourCycle(first)},
            {"mu", temperature(first)},
            {"nu", numbers(first)},
        };
        for (String[] k : keywords) {
            if (k[1] == null) continue;
            b.setUnicodeLocaleKeyword(k[0], k[1]);
            prefs.add(k[0] + "-" + k[1]);
        }
        if (!prefs.isEmpty()) {
            how.append(", regional preferences ").append(String.join(" ", prefs));
        }
        locales.set(0, b.build());
        Set<Locale> unique = new LinkedHashSet<>(locales);
        sLocales = new LocaleList(unique.toArray(new Locale[0]));
        sSource = how.toString();
    }

    /**
     * Windows' calendar, when it is not the locale's own (CAL_* ids: 1 and 2 Gregorian,
     * 3 Japanese era, 4 Taiwan (ROC), 6 Hijri, 7 Thai Buddhist, 8 Hebrew, 23 Umm al-Qura).
     */
    private static String calendar(Locale locale) {
        String value = windowsInfo(LOCALE_ICALENDARTYPE);
        if (value == null) return null;
        String want;
        switch (value.trim()) {
            case "1":
            case "2":
            case "9":
            case "10":
            case "11":
            case "12":
                want = "gregorian";  // Gregorian in its localized, US, French, Arabic... forms
                break;
            case "3":
                want = "japanese";
                break;
            case "4":
                want = "roc";
                break;
            case "6":
                want = "islamic";
                break;
            case "7":
                want = "buddhist";
                break;
            case "8":
                want = "hebrew";
                break;
            case "23":
                want = "islamic-umalqura";
                break;
            default:
                return null;  // Korean Tangun era and others: no CLDR calendar
        }
        String own = Calendar.getInstance(ULocale.forLocale(locale)).getType();
        return want.equals(own) ? null : want;
    }

    /** Windows' first day of the week, when it is not the locale's own. */
    private static String firstDayOfWeek(Locale locale) {
        String value = windowsInfo(LOCALE_IFIRSTDAYOFWEEK);
        if (value == null) return null;
        int day;
        try {
            day = Integer.parseInt(value.trim());  // 0 = Monday ... 6 = Sunday
        } catch (NumberFormatException e) {
            return null;
        }
        if (day < 0 || day > 6) return null;
        // ICU: Calendar.SUNDAY = 1 ... SATURDAY = 7.
        int icuDay = day == 6 ? Calendar.SUNDAY : day + 2;
        int own = Calendar.getInstance(ULocale.forLocale(locale)).getFirstDayOfWeek();
        return own == icuDay ? null : DAYS[day];
    }

    /** Windows' hour cycle, when it is not the locale's own. */
    private static String hourCycle(Locale locale) {
        String format = windowsInfo(LOCALE_SSHORTTIME);
        if (format == null) format = windowsInfo(LOCALE_STIMEFORMAT);
        if (format == null) return null;
        boolean windows24 = hasHour24(format);
        return windows24 == localeIs24Hour(locale) ? null : windows24 ? "h23" : "h12";
    }

    /**
     * The temperature unit of Windows' measurement system (0 metric: Celsius, 1 U.S.:
     * Fahrenheit), when it is not the locale's own (Fahrenheit where CLDR's measurement
     * system is U.S.).
     */
    private static String temperature(Locale locale) {
        String value = windowsInfo(LOCALE_IMEASURE);
        if (value == null) return null;
        String want;
        switch (value.trim()) {
            case "0":
                want = "celsius";
                break;
            case "1":
                want = "fahrenhe";
                break;
            default:
                return null;
        }
        String own;
        try {
            own = LocaleData.getMeasurementSystem(ULocale.forLocale(locale))
                    == LocaleData.MeasurementSystem.US ? "fahrenhe" : "celsius";
        } catch (RuntimeException e) {
            return null;
        }
        return want.equals(own) ? null : want;
    }

    /**
     * Windows' digits, when they are not the locale's own: native digits
     * (LOCALE_IDIGITSUBSTITUTION 2) or Latin ones (1); 0 (by context) leaves the locale's.
     */
    private static String numbers(Locale locale) {
        String value = windowsInfo(LOCALE_IDIGITSUBSTITUTION);
        if (value == null) return null;
        ULocale u = ULocale.forLocale(locale);
        String own = NumberingSystem.getInstance(u).getName();
        String want;
        switch (value.trim()) {
            case "1":
                want = "latn";
                break;
            case "2":
                want = NumberingSystem.getInstance(u.setKeywordValue("numbers", "native"))
                        .getName();
                break;
            default:
                return null;
        }
        return want == null || want.equals(own) ? null : want;
    }

    /** Whether a Windows time format ("HH:mm", "h:mm tt") counts 24 hours. */
    private static boolean hasHour24(String format) {
        boolean quoted = false;
        for (char c : format.toCharArray()) {
            if (c == '\'') quoted = !quoted;
            else if (!quoted && c == 'H') return true;
        }
        return false;
    }

    /** Whether the locale's own short time pattern counts 24 hours. */
    private static boolean localeIs24Hour(Locale locale) {
        String pattern = android.icu.text.DateTimePatternGenerator
                .getInstance(ULocale.forLocale(locale)).getBestPattern("jm");
        return pattern.indexOf('H') >= 0 || pattern.indexOf('k') >= 0;
    }

    private static String windowsInfo(int type) {
        try {
            return nativeLocaleInfo(type);
        } catch (LinkageError e) {
            return null;
        }
    }

    /** GetUserPreferredUILanguages(MUI_LANGUAGE_NAME): language tags, in order. */
    private static native String[] nativePreferredUiLanguages();

    /** GetUserDefaultLocaleName: the regional format, a language tag. */
    private static native String nativeUserLocaleName();

    /** GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type), the user's overrides included. */
    private static native String nativeLocaleInfo(int type);
}
