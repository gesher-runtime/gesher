/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.gcm;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;

import java.util.List;

public class GcmReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null) return;
        intent.setComponent(null);
        intent.setPackage(context.getPackageName());
        List<ResolveInfo> services = context.getPackageManager().queryIntentServices(intent, 0);
        if (services != null && !services.isEmpty()) {
            ComponentName comp = new ComponentName(services.get(0).serviceInfo.packageName,
                    services.get(0).serviceInfo.name);
            intent.setComponent(comp);
            context.startService(intent);
        }
    }
}
