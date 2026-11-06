/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.orderedbroadcast.target;

import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Process;

/** High-priority dynamic receiver deliberately finishes asynchronously. */
public final class TargetService extends Service {
    private static final String PERMISSION = "android.permission.ACCESS_NETWORK_STATE";
    private final Handler handler = new Handler();
    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            Bundle result = getResultExtras(false);
            boolean valid = isOrderedBroadcast() && getResultCode()==101 && "initial".equals(getResultData())
                    && result!=null && "initial".equals(result.getString("stage"));
            System.out.println("ordered: dynamic received ordered=" + isOrderedBroadcast() + " initial=" + valid);
            final PendingResult pending = goAsync();
            if (pending == null) throw new AssertionError("Missing PendingResult");
            handler.postDelayed(() -> {
                Bundle next = new Bundle();
                next.putString("stage","dynamic"); next.putString("trace","dynamic");
                next.putInt("dynamicPid",Process.myPid());
                pending.setResultCode(202); pending.setResultData("dynamic"); pending.setResultExtras(next);
                pending.finish();
                System.out.println("ordered: dynamic async finished code=202");
            }, 1500);
        }
    };
    @Override public void onCreate() {
        super.onCreate();
        IntentFilter filter = new IntentFilter("com.gesher.orderedbroadcast.EVENT");
        filter.setPriority(1000);
        registerReceiver(receiver, filter, PERMISSION, handler, Context.RECEIVER_EXPORTED);
        System.out.println("ordered: target service registered");
    }
    @Override public int onStartCommand(Intent intent, int flags, int id) {
        sendBroadcast(new Intent("com.gesher.orderedbroadcast.READY")
                .setPackage("com.gesher.orderedbroadcast.source"), PERMISSION);
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent intent) { return null; }
    @Override public void onDestroy() { unregisterReceiver(receiver); super.onDestroy(); }
}
