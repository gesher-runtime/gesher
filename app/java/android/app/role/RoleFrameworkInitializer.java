/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.app.role;

import android.app.SystemServiceRegistry;
import android.content.Context;

/**
 * The Role module's initializer, which SystemServiceRegistry's static initializer
 * calls (framework.jar compiles against the module's API stubs and does not contain it).
 * Registers {@link RoleManager} as {@code role}, reporting no available or held Android
 * roles on Windows.
 */
public final class RoleFrameworkInitializer {
    private RoleFrameworkInitializer() {
    }

    public static void registerServiceWrappers() {
        SystemServiceRegistry.registerContextAwareService(
                Context.ROLE_SERVICE,
                RoleManager.class,
                (SystemServiceRegistry.ContextAwareServiceProducerWithoutBinder<RoleManager>)
                        RoleManager::new);
    }
}
