/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.app.Application;

/** Reports onCreate: the app's providers are installed before it. */
public class ComponentsApplication extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        Report.line("ComponentsApplication onCreate, providers installed: "
                + ItemsProvider.sCreated);
    }
}
