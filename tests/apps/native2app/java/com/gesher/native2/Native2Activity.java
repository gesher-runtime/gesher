/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.native2;

import android.app.Activity;
import android.os.Bundle;

/**
 * Loads libnative2.so (tests/native2/native2_test.cpp) with System.loadLibrary from
 * the app's native library directory and runs its checks in onCreate (shim
 * extras, dlopen of the platform's libraries, flock, links, inotify), printing
 * each line as "Native2Activity: ...". onResume prints the verdict.
 */
public class Native2Activity extends Activity {
    static native String run(String dir, String symbols);

    private String verdict = "not run";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            System.loadLibrary("native2");
            String report = run(getFilesDir().getAbsolutePath(), "");
            for (String line : report.split("\n")) System.out.println("Native2Activity: " + line);
            verdict = report.contains("native2: all passed") ? "all passed" : "FAILED";
        } catch (Throwable t) {
            verdict = "threw " + t;
            t.printStackTrace(System.out);
        }
        System.out.flush();
    }

    @Override
    protected void onResume() {
        super.onResume();
        System.out.println("Native2Activity onResume: " + verdict);
        System.out.flush();
    }
}
