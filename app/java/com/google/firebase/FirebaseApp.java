/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.firebase;

import android.content.Context;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Entry point for Firebase SDKs.
 */
public class FirebaseApp {
    public static final String DEFAULT_APP_NAME = "[DEFAULT]";

    private static final Object sLock = new Object();
    private static final Map<String, FirebaseApp> sApps = new HashMap<>();

    private final Context mContext;
    private final String mName;
    private final FirebaseOptions mOptions;

    protected FirebaseApp(Context context, String name, FirebaseOptions options) {
        mContext = context != null ? context.getApplicationContext() : null;
        mName = name;
        mOptions = options;
    }

    public static FirebaseApp getInstance() {
        synchronized (sLock) {
            FirebaseApp defaultApp = sApps.get(DEFAULT_APP_NAME);
            if (defaultApp == null) {
                // If not explicitly initialized, return a default stand-in instance
                defaultApp = new FirebaseApp(null, DEFAULT_APP_NAME, new FirebaseOptions.Builder().build());
                sApps.put(DEFAULT_APP_NAME, defaultApp);
            }
            return defaultApp;
        }
    }

    public static FirebaseApp getInstance(String name) {
        synchronized (sLock) {
            FirebaseApp app = sApps.get(name);
            if (app == null) {
                app = new FirebaseApp(null, name, new FirebaseOptions.Builder().build());
                sApps.put(name, app);
            }
            return app;
        }
    }

    public static FirebaseApp initializeApp(Context context) {
        return initializeApp(context, new FirebaseOptions.Builder().build());
    }

    public static FirebaseApp initializeApp(Context context, FirebaseOptions options) {
        return initializeApp(context, options, DEFAULT_APP_NAME);
    }

    public static FirebaseApp initializeApp(Context context, FirebaseOptions options, String name) {
        synchronized (sLock) {
            FirebaseApp app = new FirebaseApp(context, name, options);
            sApps.put(name, app);
            return app;
        }
    }

    public Context getApplicationContext() {
        return mContext;
    }

    public String getName() {
        return mName;
    }

    public FirebaseOptions getOptions() {
        return mOptions;
    }

    public static List<FirebaseApp> getApps(Context context) {
        synchronized (sLock) {
            return new ArrayList<>(sApps.values());
        }
    }

    public boolean isDefaultApp() {
        return DEFAULT_APP_NAME.equals(getName());
    }

    public boolean isDataCollectionDefaultEnabled() {
        return false;
    }

    public void setDataCollectionDefaultEnabled(boolean enabled) {
    }

    public void setDataCollectionDefaultEnabled(Boolean enabled) {
    }

    public <T> T get(Class<T> anInterface) {
        return null;
    }

    public void delete() {
        synchronized (sLock) {
            sApps.remove(mName);
        }
    }

    public static void clearInstancesForTest() {
        synchronized (sLock) {
            sApps.clear();
        }
    }
}
