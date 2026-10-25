/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.localeapp;

import android.app.Activity;
import android.app.LocaleManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.LocaleList;
import android.os.Looper;

/**
 * Sets the app's locales ({@link #LOCALES}) with LocaleManager, as
 * AppCompatDelegate.setApplicationLocales does on API 33+. The system recreates it with
 * them (it does not handle locale changes): the new instance reports and finishes. Without
 * a recreation (a single process: no activity manager) it finishes after a while.
 * {@link ClearLocalesActivity} clears them.
 */
public class SetLocalesActivity extends Activity {
    static final String LOCALES = "he-IL";

    String locales() {
        return LOCALES;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String name = getClass().getSimpleName();
        if (savedInstanceState != null) {
            Report.line(name + " recreated (" + savedInstanceState.getString("set") + ") "
                    + Report.state(this, null));
            new Handler(Looper.getMainLooper()).post(this::finish);
            return;
        }
        Report.line(name + " before " + Report.state(this, null));
        LocaleManager lm = getSystemService(LocaleManager.class);
        lm.setApplicationLocales(LocaleList.forLanguageTags(locales()));
        Report.line(name + " setApplicationLocales(" + locales() + "): getApplicationLocales "
                + lm.getApplicationLocales().toLanguageTags());
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (!isFinishing() && !isDestroyed()) {
                Report.line(name + " was not recreated");
                finish();
            }
        }, 60_000);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putString("set", locales());
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Report.line(getClass().getSimpleName() + " onDestroy (changing configurations "
                + isChangingConfigurations() + ")");
    }
}
