/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.firebase.messaging;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/**
 * Base service for receiving Firebase Cloud Messaging events.
 */
public class FirebaseMessagingService extends Service {
    public static final String ACTION_DIRECT_BOOT_REMOTE_INTENT = "com.google.firebase.messaging.RECEIVE_DIRECT_BOOT";

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null) {
            handleIntent(intent);
        }
        return START_NOT_STICKY;
    }

    public void handleIntent(Intent intent) {
        String action = intent.getAction();
        if ("com.google.android.c2dm.intent.RECEIVE".equals(action)
                || "com.google.firebase.MESSAGING_EVENT".equals(action)) {
            String messageType = intent.getStringExtra("message_type");
            if (messageType == null || "gcm".equals(messageType)) {
                onMessageReceived(new RemoteMessage(intent.getExtras()));
            } else if ("deleted_messages".equals(messageType)) {
                onDeletedMessages();
            } else if ("send_event".equals(messageType)) {
                onMessageSent(intent.getStringExtra("google.message_id"));
            } else if ("send_error".equals(messageType)) {
                onSendError(intent.getStringExtra("google.message_id"),
                        new Exception(intent.getStringExtra("error")));
            }
        } else if ("com.google.firebase.messaging.NEW_TOKEN".equals(action)) {
            String token = intent.getStringExtra("token");
            if (token != null) {
                onNewToken(token);
            }
        }
    }

    public void onMessageReceived(RemoteMessage message) {
    }

    public void onDeletedMessages() {
    }

    public void onMessageSent(String msgId) {
    }

    public void onSendError(String msgId, Exception exception) {
    }

    public void onNewToken(String token) {
    }
}
