/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.namedprocesslauncher;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.Process;

/** Default-process sibling stays alive after the requested named process exits. */
public final class SiblingService extends Service {
    @Override public void onCreate() {
        super.onCreate();
        System.out.println("named-launch: sibling-ready pid="+Process.myPid()+" process="+android.app.Application.getProcessName());
    }
    @Override public int onStartCommand(Intent intent,int flags,int id){return START_NOT_STICKY;}
    @Override public IBinder onBind(Intent intent){return null;}
}
