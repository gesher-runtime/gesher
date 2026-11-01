/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.gcm;

import android.app.Service;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;

public class GcmListenerService extends Service {
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && "com.google.android.c2dm.intent.RECEIVE".equals(intent.getAction())) {
            handleIntent(intent);
        }
        return START_NOT_STICKY;
    }

    private void handleIntent(Intent intent) {
        String messageType = intent.getStringExtra("message_type");
        if (messageType == null || GoogleCloudMessaging.MESSAGE_TYPE_MESSAGE.equals(messageType)) {
            Bundle data = intent.getExtras();
            if (data == null) data = new Bundle();
            data.remove("message_type");
            String from = data.getString("from");
            data.remove("from");
            onMessageReceived(from, data);
        } else if (GoogleCloudMessaging.MESSAGE_TYPE_DELETED.equals(messageType)) {
            onDeletedMessages();
        } else if (GoogleCloudMessaging.MESSAGE_TYPE_SEND_EVENT.equals(messageType)) {
            onMessageSent(intent.getStringExtra("google.message_id"));
        } else if (GoogleCloudMessaging.MESSAGE_TYPE_SEND_ERROR.equals(messageType)) {
            onSendError(intent.getStringExtra("google.message_id"), intent.getStringExtra("error"));
        }
    }

    public void onMessageReceived(String from, Bundle data) {
    }

    public void onDeletedMessages() {
    }

    public void onMessageSent(String msgId) {
    }

    public void onSendError(String msgId, String error) {
    }
}
