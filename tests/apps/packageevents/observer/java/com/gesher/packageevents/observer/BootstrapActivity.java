/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.packageevents.observer;
import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
public final class BootstrapActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);startService(new Intent(this,ObserverService.class));finish();System.exit(0);
    }
}
