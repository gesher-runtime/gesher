/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.lifecycle;

import android.app.Activity;
import android.content.res.TypedArray;
import android.os.Bundle;
import android.util.Log;

/**
 * Reports each lifecycle callback on stdout (and in the log), and on resume what it read
 * from its resources and theme, so a test can check the manifest, the APK's resources
 * and framework-res.apk were all loaded. It sets no content view: gesher-app runs it
 * headless.
 */
public class LifecycleActivity extends Activity {
    private static final String TAG = "LifecycleActivity";

    static void report(String message) {
        System.out.println(message);
        System.out.flush();
        Log.i(TAG, message);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        report("LifecycleActivity onCreate, savedInstanceState=" + savedInstanceState);
    }

    @Override
    protected void onStart() {
        super.onStart();
        report("LifecycleActivity onStart");
    }

    @Override
    protected void onPostCreate(Bundle savedInstanceState) {
        super.onPostCreate(savedInstanceState);
        report("LifecycleActivity onPostCreate");
    }

    @Override
    protected void onResume() {
        super.onResume();
        String marker;
        boolean actionBar;
        // In ascending order, as obtainStyledAttributes wants them.
        TypedArray a = getTheme().obtainStyledAttributes(
                new int[] {android.R.attr.windowActionBar, R.attr.gesherMarker});
        try {
            actionBar = a.getBoolean(0, true);
            marker = a.getString(1);
        } finally {
            a.recycle();
        }
        report("LifecycleActivity onResume, label=" + getTitle()
                + ", greeting=" + getString(R.string.greeting)
                + ", theme attr=" + marker
                + ", windowActionBar=" + actionBar);
    }

    @Override
    protected void onPause() {
        super.onPause();
        report("LifecycleActivity onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        report("LifecycleActivity onStop");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        report("LifecycleActivity onDestroy");
    }
}
