/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.wifi;

/**
 * Register the bounded unsupported-controller wrapper during SystemServiceRegistry
 * initialization. This does not declare Android Wi-Fi hardware support.
 */
public final class WifiFrameworkInitializer {
    private WifiFrameworkInitializer() {
    }

    public static void registerServiceWrappers() {
        android.app.SystemServiceRegistry.registerContextAwareService(
                android.content.Context.WIFI_SERVICE, WifiManager.class, WifiManager::new);
    }
}
