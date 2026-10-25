/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.localeapp;

import android.app.LocaleConfig;
import android.app.LocaleManager;
import android.content.Context;
import android.content.res.Configuration;
import android.os.LocaleList;
import android.os.Process;
import android.util.Log;

import java.text.NumberFormat;
import java.util.Calendar;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

/** Lines on stdout (the package's log), prefixed "localeapp: ", for the tests to check. */
final class Report {
    /** 2026-03-04 13:05 UTC: the date and time the reports format. */
    private static final long WHEN = 1772629500000L;

    private Report() {
    }

    static void line(String message) {
        System.out.println("localeapp: " + message);
        System.out.flush();
        Log.i("localeapp", message);
    }

    /**
     * What the app sees of the locale, in {@code config} (its resources' configuration when
     * null): "locales=... config=... app=... system=... greeting=... dir=ltr|rtl
     * number=... date=... time=... 24h=... firstDay=... localeConfig=... pid=...".
     */
    static String state(Context context, Configuration config) {
        if (config == null) config = context.getResources().getConfiguration();
        StringBuilder s = new StringBuilder();
        s.append("locales=").append(LocaleList.getDefault().toLanguageTags());
        s.append(" config=").append(config.getLocales().toLanguageTags());
        LocaleManager lm = context.getSystemService(LocaleManager.class);
        if (lm != null) {
            s.append(" app=[").append(lm.getApplicationLocales().toLanguageTags()).append(']');
            s.append(" system=").append(lm.getSystemLocales().toLanguageTags());
        } else {
            s.append(" app=(no LocaleManager)");
        }
        s.append(" greeting=").append(context.getString(R.string.greeting));
        s.append(" dir=").append(config.getLayoutDirection() == android.view.View.LAYOUT_DIRECTION_RTL
                ? "rtl" : "ltr");
        Locale locale = Locale.getDefault();
        s.append(" number=").append(NumberFormat.getInstance(locale).format(1234567.5));
        java.text.DateFormat date = android.text.format.DateFormat.getDateFormat(context);
        date.setTimeZone(TimeZone.getTimeZone("UTC"));
        s.append(" date=").append(date.format(new Date(WHEN)));
        java.text.DateFormat time = android.text.format.DateFormat.getTimeFormat(context);
        time.setTimeZone(TimeZone.getTimeZone("UTC"));
        s.append(" time=").append(time.format(new Date(WHEN)));
        s.append(" 24h=").append(android.text.format.DateFormat.is24HourFormat(context));
        s.append(" firstDay=").append(Calendar.getInstance(locale).getFirstDayOfWeek());
        try {
            LocaleConfig lc = new LocaleConfig(context);
            s.append(" localeConfig=").append(lc.getStatus() == LocaleConfig.STATUS_SUCCESS
                    ? lc.getSupportedLocales().toLanguageTags() : "status " + lc.getStatus());
        } catch (Throwable t) {
            s.append(" localeConfig=(").append(t).append(')');
        }
        s.append(" pid=").append(Process.myPid());
        return s.toString();
    }
}
