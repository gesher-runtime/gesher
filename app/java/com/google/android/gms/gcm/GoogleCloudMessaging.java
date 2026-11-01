/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.gcm;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

import java.io.IOException;
import java.util.Collections;
import java.util.UUID;

/**
 * Legacy Google Cloud Messaging (GCM) client.
 */
public class GoogleCloudMessaging {
    public static final String MESSAGE_TYPE_MESSAGE = "gcm";
    public static final String MESSAGE_TYPE_DELETED = "deleted_messages";
    public static final String MESSAGE_TYPE_SEND_ERROR = "send_error";
    public static final String MESSAGE_TYPE_SEND_EVENT = "send_event";

    public static final String ERROR_SERVICE_NOT_AVAILABLE = "SERVICE_NOT_AVAILABLE";
    public static final String ERROR_MAIN_THREAD = "MAIN_THREAD";

    private static GoogleCloudMessaging sInstance;
    private final Context mContext;

    private GoogleCloudMessaging(Context context) {
        mContext = context != null ? context.getApplicationContext() : null;
    }

    public static synchronized GoogleCloudMessaging getInstance(Context context) {
        if (sInstance == null) {
            sInstance = new GoogleCloudMessaging(context);
        }
        return sInstance;
    }

    public synchronized String register(String... senderIds) throws IOException {
        String sender = (senderIds != null && senderIds.length > 0) ? senderIds[0] : "default";
        return "gcm_reg_token_gesher_" + sender + "_" + UUID.randomUUID().toString().substring(0, 8);
    }

    public synchronized void unregister() throws IOException {
    }

    public void send(String to, String msgId, Bundle data) throws IOException {
        send(to, msgId, -1L, data);
    }

    public void send(String to, String msgId, long timeToLive, Bundle data) throws IOException {
    }

    public void close() {
    }

    public String getMessageType(Intent intent) {
        if (!"com.google.android.c2dm.intent.RECEIVE".equals(intent.getAction())) {
            return null;
        }
        String type = intent.getStringExtra("message_type");
        return type != null ? type : MESSAGE_TYPE_MESSAGE;
    }
}
