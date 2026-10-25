/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.localeapp;

import android.app.Application;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.res.Configuration;

/**
 * The app: its configuration changes (ComponentCallbacks: the process's, not an
 * activity's) and a receiver registered for as long as the process lives (an activity's
 * would go with a relaunch): LOCALE_CHANGED, CONFIGURATION_CHANGED, TIME_SET.
 */
public final class LocaleApp extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        IntentFilter filter = new IntentFilter();
        filter.addAction(Intent.ACTION_LOCALE_CHANGED);
        filter.addAction(Intent.ACTION_CONFIGURATION_CHANGED);
        filter.addAction(Intent.ACTION_TIME_CHANGED);
        registerReceiver(new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String extra = intent.hasExtra(Intent.EXTRA_TIME_PREF_24_HOUR_FORMAT)
                        ? " 24-hour extra " + intent.getIntExtra(
                                Intent.EXTRA_TIME_PREF_24_HOUR_FORMAT, -1) : "";
                Report.line("LocaleApp registered receiver got " + intent.getAction() + extra
                        + " (locales " + android.os.LocaleList.getDefault().toLanguageTags() + ")");
            }
        }, filter, Context.RECEIVER_NOT_EXPORTED);
        Report.line("LocaleApp onCreate " + Report.state(this, null));
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        Report.line("LocaleApp onConfigurationChanged " + newConfig.getLocales().toLanguageTags()
                + " " + Report.state(this, newConfig));
    }
}
