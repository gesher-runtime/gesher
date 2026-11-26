/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.qa.admission;

public final class MainActivity extends android.app.Activity {
    @Override
    protected void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        System.out.println("EDITION_ADMISSION_ACTIVITY_ENTERED");
        finish();
    }
}
