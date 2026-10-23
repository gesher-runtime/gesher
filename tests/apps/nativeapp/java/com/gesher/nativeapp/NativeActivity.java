/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.nativeapp;

import android.app.Activity;
import android.os.Bundle;

/**
 * Loads the APK's native library in onCreate and calls into it: a string made by C++
 * code, a C++ exception thrown and caught inside the library, and a call from the
 * library back into this Activity. onResume prints what it got, so a test can check
 * the native code ran through gesher-app.
 */
public class NativeActivity extends Activity {
    private String result = "not run";

    static native String stringFromJNI();

    native int callBack(int x);

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
    }

    @Override
    protected void onResume() {
        super.onResume();
        System.out.println("NativeActivity onResume, native=" + result);
        System.out.flush();
    }
}
