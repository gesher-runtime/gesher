/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.localeapp;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** The manifest's receiver of LOCALE_CHANGED (an implicit broadcast apps may still declare). */
public final class LocaleReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        Report.line("LocaleReceiver (manifest) got " + intent.getAction() + " (locales "
                + android.os.LocaleList.getDefault().toLanguageTags() + ")");
    }
}
