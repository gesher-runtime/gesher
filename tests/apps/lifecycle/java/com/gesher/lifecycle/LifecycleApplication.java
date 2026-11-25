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
        LifecycleActivity.report("LifecycleApplication process entry: " + processEntry());
    }

    /**
     * The bottom frames of the main thread's stack, as an Android app process has them
     * (ActivityThread.main, called by RuntimeInit's caller from ZygoteInit.main), from the
     * innermost of the four.
     */
    static String processEntry() {
        StackTraceElement[] frames = new Throwable().getStackTrace();
        StringBuilder entry = new StringBuilder();
        for (int i = Math.max(0, frames.length - 4); i < frames.length; i++) {
            if (entry.length() > 0) entry.append(" < ");
            entry.append(frames[i].getClassName()).append('.').append(frames[i].getMethodName());
        }
        return entry.toString();
    }
}
