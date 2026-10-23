/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.lifecycle;

import android.app.Application;

/** The app's Application class, named in the manifest: its onCreate() runs first. */
public class LifecycleApplication extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        LifecycleActivity.report("LifecycleApplication onCreate, package=" + getPackageName()
                + ", app_name=" + getString(R.string.app_name));
    }
}
