/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.media;

/**
 * The Media module's initializer (framework.jar compiles against the module's API stubs
 * and does not contain it). SystemServiceRegistry's static initializer calls
 * {@link #registerServiceWrappers}, which registers nothing: the module's services are not
 * served. ActivityThread.main ({@code initializeMainlineModules}) hands it the process's
 * service manager once, as the module's own initializer keeps it (ADR 0098).
 */
public final class MediaFrameworkInitializer {
    private static volatile MediaServiceManager sServiceManager;

    private MediaFrameworkInitializer() {
    }

    public static void registerServiceWrappers() {
    }

    /** As the module's: set once per process. */
    public static void setMediaServiceManager(MediaServiceManager serviceManager) {
        if (sServiceManager != null) {
            throw new IllegalStateException("setMediaServiceManager called twice!");
        }
        if (serviceManager == null) throw new NullPointerException("serviceManager");
        sServiceManager = serviceManager;
    }

    public static MediaServiceManager getMediaServiceManager() {
        return sServiceManager;
    }
}
