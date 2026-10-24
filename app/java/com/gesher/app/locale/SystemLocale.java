/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.locale;

import android.icu.text.NumberingSystem;
import android.icu.util.Calendar;
import android.icu.util.ULocale;
import android.os.LocaleList;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * The device's locales, from Windows (docs/fonts.md, "Locale";
 * docs/decisions/0044-rtl-scripts-and-locale.md). What a device's Settings app keeps in
 * {@code persist.sys.locale} and the system server puts in every process's
 * Configuration, Gesher takes from the user's Windows settings:
 *
 * <ul>
 * <li>the locale list is Windows' preferred UI (display) languages, in order
 *     ({@code GetUserPreferredUILanguages});
 * <li>the first locale takes the user's regional format ({@code GetUserDefaultLocaleName})
 *     when it is the same language (an en-US display with en-GB formats is en-GB);
 * <li>the regional settings that differ from that locale's own defaults become Unicode
 *     extensions on it, as Android 14+ keeps its regional preferences: the first day of
 *     the week ({@code -u-fw-}, {@code LOCALE_IFIRSTDAYOFWEEK}), the hour cycle
 *     ({@code -u-hc-h23}/{@code h12}, the time format), the digits
 *     ({@code -u-nu-}, {@code LOCALE_IDIGITSUBSTITUTION}).
 * </ul>
 *
 * <p>{@code $GESHER_LOCALES} (comma-separated language tags, e.g. {@code he-IL,en-US})
 * replaces the list as it is. With neither, the list is en-US. The layout direction
 * follows the first locale (Configuration.setLayoutDirection): right to left for Hebrew,
 * Arabic, Persian, Urdu...
 */
public final class SystemLocale {
    /** The environment variable that sets the locale list (language tags, ',' between). */
    public static final String ENV = "GESHER_LOCALES";

    // GetLocaleInfoEx's LCTYPEs.
    private static final int LOCALE_STIMEFORMAT = 0x1003;
    private static final int LOCALE_IFIRSTDAYOFWEEK = 0x100C;
    private static final int LOCALE_IDIGITSUBSTITUTION = 0x1014;
    private static final int LOCALE_SSHORTTIME = 0x79;

    private static final String[] DAYS = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

    private static LocaleList sLocales;
    private static String sSource;
    private static Boolean sIs24Hour;

    private SystemLocale() {
    }

    /** The device's locale list (computed once per process). */
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
        String fw = firstDayOfWeek(first);
        if (fw != null) {
            b.setUnicodeLocaleKeyword("fw", fw);
            prefs.add("fw-" + fw);
        }
        String hc = hourCycle(first);
        if (hc != null) {
            b.setUnicodeLocaleKeyword("hc", hc);
            prefs.add("hc-" + hc);
        }
        String nu = numbers(first);
        if (nu != null) {
            b.setUnicodeLocaleKeyword("nu", nu);
            prefs.add("nu-" + nu);
        }
        if (!prefs.isEmpty()) {
            how.append(", regional preferences ").append(String.join(" ", prefs));
        }
        locales.set(0, b.build());
        Set<Locale> unique = new LinkedHashSet<>(locales);
        sLocales = new LocaleList(unique.toArray(new Locale[0]));
        sSource = how.toString();
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
