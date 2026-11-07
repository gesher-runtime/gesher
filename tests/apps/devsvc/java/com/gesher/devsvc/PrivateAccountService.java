/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

/** A real non-exported target that the attached cross-package probe must never start. */
public final class PrivateAccountService extends android.app.Service {
    @Override public android.os.IBinder onBind(android.content.Intent intent) {
        Report.line("account: FAIL private fixture service reached by another app");
        return new android.os.Binder();
    }
    @Override public int onStartCommand(android.content.Intent intent, int flags, int id) {
        Report.line("account: FAIL private fixture service reached by another app");
        stopSelf(); return START_NOT_STICKY;
    }
}
