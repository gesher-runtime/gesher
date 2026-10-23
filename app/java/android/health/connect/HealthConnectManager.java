/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.health.connect;

import android.content.Context;

/**
 * Placeholder for the health fitness module's HealthConnectManager, which framework.jar is
 * compiled against but does not contain (app/README.md, "Classes framework.jar does not
 * have"). AppOpsManager.permissionToOp asks it whether a permission without an app op is a
 * health permission (a provider checking a caller's permission gets there): with no health
 * module, none is. When the module's class is on the boot class path ahead of
 * gesher-app.jar, it wins.
 */
public final class HealthConnectManager {
    private HealthConnectManager() {
    }

    /** Whether {@code permission} is a health permission: none is, without the module. */
    public static boolean isHealthPermission(Context context, String permission) {
        return false;
    }
}
