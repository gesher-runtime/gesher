/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Set;

/** Frozen installation eligibility, separate from the user's runtime permission decision. */
final class InstallerPermissionAllowlist {
    static final InstallerPermissionAllowlist ALL = new InstallerPermissionAllowlist(null);
    private final Set<String> permissions; // null is SessionParams' ALL, not an explicit empty list.

    private InstallerPermissionAllowlist(Set<String> permissions) {
        this.permissions = permissions == null ? null : Collections.unmodifiableSet(permissions);
    }

    static InstallerPermissionAllowlist fromSession(PackageInstaller.SessionParams params, PermissionPolicy policy) {
        if ((params.installFlags & PackageManager.INSTALL_ALL_WHITELIST_RESTRICTED_PERMISSIONS) != 0) return ALL;
        Set<String> names = new LinkedHashSet<>();
        if (params.whitelistedRestrictedPermissions != null) {
            for (String name : new ArrayList<>(params.whitelistedRestrictedPermissions)) {
                PermissionInfo info = name == null ? null : policy.getPermissionInfo(name, null, 0);
                if (info != null && "android".equals(info.packageName) && info.isRestricted()) names.add(name);
            }
        }
        return new InstallerPermissionAllowlist(names);
    }

    boolean includes(String permission) { return permissions == null || permissions.contains(permission); }
    int installFlags() { return permissions == null ? PackageManager.INSTALL_ALL_WHITELIST_RESTRICTED_PERMISSIONS : 0; }

    void describe(PackageInstaller.SessionInfo info) {
        info.installFlags = installFlags();
        info.whitelistedRestrictedPermissions = permissions == null ? null : new ArrayList<>(permissions);
    }
}
