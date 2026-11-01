/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.firebase.messaging;

import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;
import com.google.firebase.FirebaseApp;

import java.util.UUID;

/**
 * Entry point for Firebase Cloud Messaging operations.
 */
public class FirebaseMessaging {
    public static final String INSTANCE_ID_SCOPE = "FCM";

    private static final Object sLock = new Object();
    private static FirebaseMessaging sInstance;
    private static String sToken = "fcm_token_gesher_" + UUID.randomUUID().toString().replace("-", "");

    private final FirebaseApp mApp;
    private boolean mAutoInitEnabled = true;

    FirebaseMessaging(FirebaseApp app) {
        mApp = app;
    }

    public static FirebaseMessaging getInstance() {
        return getInstance(FirebaseApp.getInstance());
    }

    public static synchronized FirebaseMessaging getInstance(FirebaseApp app) {
        synchronized (sLock) {
            if (sInstance == null) {
                sInstance = new FirebaseMessaging(app);
            }
            return sInstance;
        }
    }

    public Task<String> getToken() {
        return Tasks.forResult(sToken);
    }

    public Task<Void> deleteToken() {
        synchronized (sLock) {
            sToken = "fcm_token_gesher_" + UUID.randomUUID().toString().replace("-", "");
        }
        return Tasks.forResult(null);
    }

    public Task<Void> subscribeToTopic(String topic) {
        return Tasks.forResult(null);
    }

    public Task<Void> unsubscribeFromTopic(String topic) {
        return Tasks.forResult(null);
    }

    public boolean isAutoInitEnabled() {
        return mAutoInitEnabled;
    }

    public void setAutoInitEnabled(boolean enable) {
        mAutoInitEnabled = enable;
    }

    public void send(RemoteMessage message) {
    }

    public static void setTokenForTest(String token) {
        synchronized (sLock) {
            sToken = token;
        }
    }
}
