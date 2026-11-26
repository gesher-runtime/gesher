/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.firebase.messaging;

import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;
import com.google.firebase.FirebaseApp;

import java.io.IOException;

/**
 * Firebase Cloud Messaging for the gmsapp test fixture (app/build.sh keeps these
 * client-library classes out of gesher-app.jar). It has no registration transport, so
 * every token or topic request fails as Firebase's GmsRpc reports a registration with no
 * answer: IOException("SERVICE_NOT_AVAILABLE"). It never invents a token (issue #335).
 */
public class FirebaseMessaging {
    public static final String INSTANCE_ID_SCOPE = "FCM";

    private static final Object sLock = new Object();
    private static FirebaseMessaging sInstance;
    /** Firebase GmsRpc.ERROR_SERVICE_NOT_AVAILABLE. */
    static final String ERROR_SERVICE_NOT_AVAILABLE = "SERVICE_NOT_AVAILABLE";

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

    private static <T> Task<T> unavailable() {
        return Tasks.forException(new IOException(ERROR_SERVICE_NOT_AVAILABLE));
    }

    public Task<String> getToken() {
        return unavailable();
    }

    public Task<Void> deleteToken() {
        return unavailable();
    }

    public Task<Void> subscribeToTopic(String topic) {
        return unavailable();
    }

    public Task<Void> unsubscribeFromTopic(String topic) {
        return unavailable();
    }

    public boolean isAutoInitEnabled() {
        return mAutoInitEnabled;
    }

    public void setAutoInitEnabled(boolean enable) {
        mAutoInitEnabled = enable;
    }

    public void send(RemoteMessage message) {
    }
}
