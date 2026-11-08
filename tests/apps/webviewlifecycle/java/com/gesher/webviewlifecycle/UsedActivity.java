/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.webviewlifecycle;

import android.app.Activity;
import android.os.Bundle;

public final class UsedActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Checks.report(this,"disable-after-use",()->{
            Checks.initialized();
            Object provider=Checks.provider();
            Checks.installAgain(); Checks.installAgain();
            Checks.require(Checks.provider()==provider);
            Checks.disableAfterUse(); Checks.disableAfterUse();
            Checks.require(Checks.provider()==provider);
        });
    }
}
