/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.packageevents.target;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
public final class ReplacedReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        try {
            long version=context.getPackageManager().getPackageInfo(context.getPackageName(),0).getLongVersionCode();
            if(!Intent.ACTION_MY_PACKAGE_REPLACED.equals(intent.getAction()) || version!=2) {
                System.out.println("packageevents-target: FAIL replacement");return;
            }
            System.out.println("packageevents-target: MY_REPLACED version=2");
        } catch(Exception e) { System.out.println("packageevents-target: FAIL package-query"); }
    }
}
