/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.web3app;

import android.app.Activity;
import android.net.ConnectivityManager;
import android.os.Bundle;

/** The same selected-network/NDK checks in gesher-app's single process. */
public final class BindingActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        ConnectivityManager manager = getSystemService(ConnectivityManager.class);
        new Thread(() -> {
            try {
                NetActivity.binding(manager, NetActivity.nativeLibrary(getIntent()));
            } catch (Throwable failure) {
                Web3.log("net failed: " + failure);
            }
            Web3.log("net done");
            runOnUiThread(this::finish);
        }, "web3-network-binding").start();
    }
}
