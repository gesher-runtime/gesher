/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.os.Bundle;

import java.util.Map;
import java.util.UUID;

/**
 * Server-side support for Google Play Services (GMS) and Firebase Cloud Messaging (FCM).
 * Handles registration tokens and routing of push notification broadcasts.
 */
public final class GmsService {
    public static final String ACTION_C2DM_RECEIVE = "com.google.android.c2dm.intent.RECEIVE";
    public static final String ACTION_C2DM_REGISTRATION = "com.google.android.c2dm.intent.REGISTRATION";
    public static final String ACTION_C2DM_REGISTER = "com.google.android.c2dm.intent.REGISTER";
    public static final String ACTION_C2DM_UNREGISTER = "com.google.android.c2dm.intent.UNREGISTER";

    public static final String EXTRA_REGISTRATION_ID = "registration_id";
    public static final String EXTRA_ERROR = "error";
    public static final String EXTRA_FROM = "from";
    public static final String EXTRA_COLLAPSE_KEY = "collapse_key";
    public static final String EXTRA_MESSAGE_ID = "google.message_id";
    public static final String EXTRA_SENT_TIME = "google.sent_time";

    private GmsService() {
    }

    public static Intent createReceiveIntent(String targetPackage, Map<String, String> data,
            String from, String messageId, String collapseKey) {
        Intent intent = new Intent(ACTION_C2DM_RECEIVE);
        if (targetPackage != null) {
            intent.setPackage(targetPackage);
        }
        Bundle extras = new Bundle();
        if (from != null) extras.putString(EXTRA_FROM, from);
        if (messageId != null) {
            extras.putString(EXTRA_MESSAGE_ID, messageId);
        } else {
            extras.putString(EXTRA_MESSAGE_ID, "msg_" + UUID.randomUUID().toString());
        }
        if (collapseKey != null) extras.putString(EXTRA_COLLAPSE_KEY, collapseKey);
        extras.putLong(EXTRA_SENT_TIME, System.currentTimeMillis());

        if (data != null) {
            for (Map.Entry<String, String> entry : data.entrySet()) {
                extras.putString(entry.getKey(), entry.getValue());
            }
        }
        intent.putExtras(extras);
        return intent;
    }

    public static Intent createRegistrationIntent(String targetPackage, String registrationId) {
        Intent intent = new Intent(ACTION_C2DM_REGISTRATION);
        if (targetPackage != null) {
            intent.setPackage(targetPackage);
        }
        intent.putExtra(EXTRA_REGISTRATION_ID,
                registrationId != null ? registrationId : "fcm_token_gesher_" + UUID.randomUUID().toString());
        return intent;
    }
}
