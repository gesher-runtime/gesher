/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.devsvc;

/** Declared foreground-service identity for the isolated notification-channel contract. */
public final class NotificationForegroundService extends android.app.Service {
    @Override public android.os.IBinder onBind(android.content.Intent intent) { return null; }
}
