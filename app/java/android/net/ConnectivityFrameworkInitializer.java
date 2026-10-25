/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.app.SystemServiceRegistry;
import android.content.Context;

/**
 * The Connectivity module's initializer, which SystemServiceRegistry's static initializer
 * calls (framework.jar compiles against the module's API stubs and does not contain it).
 * It registers {@link ConnectivityManager} as {@code connectivity}, the client of the
 * connectivity service that describes Windows' networks (docs/network.md); the module's
 * other services are not registered.
 */
public final class ConnectivityFrameworkInitializer {
    private ConnectivityFrameworkInitializer() {
    }

    public static void registerServiceWrappers() {
        SystemServiceRegistry.registerContextAwareService(Context.CONNECTIVITY_SERVICE,
                ConnectivityManager.class,
                (SystemServiceRegistry.ContextAwareServiceProducerWithoutBinder<ConnectivityManager>)
                        ConnectivityManager::new);
    }
}
