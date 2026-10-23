/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.ondevicepersonalization;

/**
 * No-op stand-in for a mainline module's initializer, which SystemServiceRegistry's
 * static initializer calls. framework.jar compiles against the module's API stubs and
 * does not contain it; this registers none of the module's services.
 */
public final class OnDevicePersonalizationFrameworkInitializer {
    private OnDevicePersonalizationFrameworkInitializer() {
    }

    public static void registerServiceWrappers() {
    }
}
