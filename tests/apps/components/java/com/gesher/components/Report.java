/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.util.Log;

/** Lines on stdout (and in the log), prefixed "components: ", for the tests to check. */
final class Report {
    private Report() {
    }

    static void line(String message) {
        System.out.println("components: " + message);
        System.out.flush();
        Log.i("components", message);
    }
}
