/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.content.pm.PackageManager;
import android.os.UserHandle;

/** Android 16 exact-alarm authority, shared by scheduling, queries and Settings. */
final class ExactAlarmAccess {
    static final String SCHEDULE = "android.permission.SCHEDULE_EXACT_ALARM";
    static final String USE = "android.permission.USE_EXACT_ALARM";

    private ExactAlarmAccess() { }

    // These are the pinned AOSP change defaults. CompatChanges has no per-app
    // overrides: REQUIRE_EXACT_ALARM_PERMISSION starts at S; USE and default
    // denial start at T. A manifest permission and its app-op are separate state.
    static boolean requiresPermission(ApkPackage pkg) {
        return pkg != null && pkg.parsed.getTargetSdkVersion() >= 31;
    }

    static boolean exempt(ApkPackage pkg) {
        // DeviceIdleController currently has no configured power allowlists.
        // A Windows process without Doze restrictions is not an allowlist member.
        return pkg != null && UserHandle.isCore(pkg.uid());
    }

    static boolean hasUse(PackageManagerService pm, ApkPackage pkg) {
        return pkg != null && pkg.parsed.getTargetSdkVersion() >= 33
                && pm.permissions().checkUidPermission(pkg.uid(), USE, 0)
                        == PackageManager.PERMISSION_GRANTED;
    }

    private static boolean scheduleCandidate(PackageManagerService pm, ApkPackage pkg) {
        String[] names = pm.getPackagesForUid(pkg.uid());
        if (names != null) for (String name : names) {
            ApkPackage member = pm.get(name);
            if (member != null && member.parsed.getRequestedPermissions().contains(SCHEDULE)) return true;
        }
        return false;
    }

    static boolean hasSchedule(PackageManagerService pm, ApkPackage pkg) {
        return pkg != null && hasSchedule(pm, pkg, pm.appOps().checkRawMode(
                AppOpsManager.OP_SCHEDULE_EXACT_ALARM, pkg.uid(), pkg.packageName));
    }

    static boolean hasSchedule(PackageManagerService pm, ApkPackage pkg, int mode) {
        if (!requiresPermission(pkg) || !scheduleCandidate(pm, pkg)) {
            return false;
        }
        if (mode == AppOpsManager.MODE_ALLOWED) return true;
        if (mode != AppOpsManager.MODE_DEFAULT) return false;
        return pkg.parsed.getTargetSdkVersion() < 33
                || pm.permissions().checkUidPermission(pkg.uid(), SCHEDULE, 0)
                        == PackageManager.PERMISSION_GRANTED;
    }

    static boolean canSchedule(PackageManagerService pm, ApkPackage pkg) {
        return pkg != null && (!requiresPermission(pkg) || exempt(pkg)
                || hasUse(pm, pkg) || hasSchedule(pm, pkg));
    }

    static boolean editable(PackageManagerService pm, ApkPackage pkg) {
        return requiresPermission(pkg) && pkg.parsed.getRequestedPermissions().contains(SCHEDULE)
                && !(pkg.parsed.getTargetSdkVersion() >= 33
                    && pkg.parsed.getRequestedPermissions().contains(USE)) && !exempt(pkg);
    }
}
