/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.os;

/**
 * The Profiling module's initializer (framework.jar compiles against the module's API stubs
 * and does not contain it). SystemServiceRegistry's static initializer calls
 * {@link #registerServiceWrappers}, which registers nothing: the module's services are not
 * served. ActivityThread.main ({@code initializeMainlineModules}) hands it the process's
 * service manager once, as the module's own initializer keeps it (ADR 0098).
 */
public final class ProfilingFrameworkInitializer {
    private static volatile ProfilingServiceManager sServiceManager;

    private ProfilingFrameworkInitializer() {
    }

    public static void registerServiceWrappers() {
    }

    /** As the module's: set once per process. */
    public static void setProfilingServiceManager(ProfilingServiceManager serviceManager) {
        if (sServiceManager != null) {
            throw new IllegalStateException("setProfilingServiceManager called twice!");
        }
        if (serviceManager == null) throw new NullPointerException("serviceManager");
        sServiceManager = serviceManager;
    }

    public static ProfilingServiceManager getProfilingServiceManager() {
        return sServiceManager;
    }
}
