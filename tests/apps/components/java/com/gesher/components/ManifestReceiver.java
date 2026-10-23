/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** A manifest receiver, reached by an explicit broadcast: a new instance per broadcast. */
public class ManifestReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        Report.line("ManifestReceiver onReceive " + intent.getAction() + " ("
                + context.getClass().getSimpleName() + ")");
        MainActivity.advance();
    }
}
