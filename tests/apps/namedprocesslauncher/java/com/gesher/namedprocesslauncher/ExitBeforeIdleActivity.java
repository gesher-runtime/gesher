/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.namedprocesslauncher;

import android.app.Activity;
import android.os.Bundle;

public final class ExitBeforeIdleActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        System.out.println("named-launch: exiting before idle");
        System.out.flush();
        System.exit(23);
    }
}
