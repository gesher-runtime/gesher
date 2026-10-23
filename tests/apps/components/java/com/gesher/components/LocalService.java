/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;

/** A bound service returning a local Binder, as the platform's "local service" sample. */
public class LocalService extends Service {
    /** The Binder onBind returns: in the same process, clients get this very object. */
    public class LocalBinder extends Binder {
        LocalService getService() {
            return LocalService.this;
        }
    }

    private final IBinder mBinder = new LocalBinder();

    int add(int a, int b) {
        return a + b;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Report.line("LocalService onCreate");
    }

    @Override
    public IBinder onBind(Intent intent) {
        Report.line("LocalService onBind");
        return mBinder;
    }

    @Override
    public boolean onUnbind(Intent intent) {
        Report.line("LocalService onUnbind");
        return false;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Report.line("LocalService onDestroy");
        MainActivity.advance();
    }
}
