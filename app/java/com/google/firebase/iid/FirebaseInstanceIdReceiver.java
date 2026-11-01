/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.firebase.iid;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;

import java.util.List;

/**
 * Manifest receiver for com.google.android.c2dm.intent.RECEIVE broadcasts.
 */
public final class FirebaseInstanceIdReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null) return;
        // Forward intent to the declared FirebaseMessagingService in this package
        Intent serviceIntent = new Intent("com.google.firebase.MESSAGING_EVENT");
        serviceIntent.setPackage(context.getPackageName());
        serviceIntent.putExtras(intent);
        List<ResolveInfo> services = context.getPackageManager().queryIntentServices(serviceIntent, 0);
        if (services != null && !services.isEmpty()) {
            ComponentName comp = new ComponentName(services.get(0).serviceInfo.packageName,
                    services.get(0).serviceInfo.name);
            serviceIntent.setComponent(comp);
            context.startService(serviceIntent);
        }
    }
}
