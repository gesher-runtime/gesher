/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.nativeapp;

import android.app.Activity;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/**
 * Loads the APK's native library in onCreate and calls into it: a string made by C++
 * code, a C++ exception thrown and caught inside the library, and a call from the
 * library back into this Activity. onResume prints what it got, so a test can check
 * the native code ran through gesher-app. Then platform checks: the library uses
 * libandroid (assets, configuration, loopers) and libc (sockets, directories,
 * std::filesystem, signals, time) and reports each; it also adds a pipe to this
 * thread's Looper, whose callback the message loop runs before the message posted
 * from onResume.
 */
public class NativeActivity extends Activity {
    private String result = "not run";

    static native String stringFromJNI();

    native int callBack(int x);

    static native String platformChecks(AssetManager assets, String dir);

    static native String mainLooperResult();

    private String platform = "not run";

    // Called by the native code.
    int square(int x) {
        return x * x;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        System.loadLibrary("nativeapp");
        result = stringFromJNI() + ", callBack(12)=" + callBack(12);
        System.out.println("NativeActivity onCreate, native=" + result);
        platform = platformChecks(getAssets(), getApplicationInfo().dataDir);
    }

    @Override
    protected void onResume() {
        super.onResume();
        System.out.println("NativeActivity onResume, native=" + result);
        System.out.println("NativeActivity platform: " + platform);
        System.out.flush();
        new Handler(Looper.getMainLooper()).post(() -> {
            System.out.println("NativeActivity main looper: " + mainLooperResult());
            System.out.flush();
        });
    }
}
