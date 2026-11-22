/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.AppOpsManager;
import android.content.pm.PackageManager;

import java.util.ArrayList;
import java.util.List;

/**
 * Android Settings' "Special app access" switches for one installed app, which Gesher's
 * Control Center shows on the app's details ({@link SettingsIntents} sends the per-app
 * screens there; there is no Settings APK).
 *
 * <p>Each switch is an app-op the user decides, as AOSP Settings writes it
 * (packages/apps/Settings, Android 16 r4): All files access
 * (ManageExternalStorageDetails: UID mode), Display over other apps (DrawOverlayDetails:
 * package mode), Install unknown apps (ExternalSourcesDetails: package mode), Modify system
 * settings (WriteSettingsDetails: package mode), Usage access (UsageAccessDetails: package
 * mode, denial MODE_IGNORED) and Media management (ManageMediaDetails: UID mode). On is
 * MODE_ALLOWED; off is the denial mode Settings writes. The modes go through
 * {@link AppOpsService} like any other mode change: persisted with the package's signing
 * identity and reported to app-op watchers ({@code OnOpChangedListener}).
 *
 * <p>As in Settings, a switch exists only for an app whose manifest requests the
 * permission. Nothing is granted unless the user turns a switch on; the permission itself
 * stays ungranted (these are appop permissions: the framework's checks read the mode, and
 * fall back to the permission only for MODE_DEFAULT).
 *
 * <p>Left out: exact alarms (AlarmManagerService allows every app exact alarms, so the
 * switch would change nothing an app observes), full-screen intents
 * (NotificationManagerService does not implement {@code canUseFullScreenIntent}), and
 * the screens with no app-op (battery optimization, locale, open by default, picture in
 * picture, long-running jobs, app usage).
 *
 * <p>Only the host shell reaches this ({@link GesherService}, whose callers must be the
 * system or the shell, never an attached app), and {@link AppOpsService} enforces
 * MANAGE_APP_OPS_MODES again on the write.
 */
final class SpecialAccess {
    private SpecialAccess() {
    }

    /** One switch: the app-op (by its public name), its permission and how Settings writes it. */
    record Kind(String opName, int op, String permission, boolean uidMode, int deniedMode) {
    }

    /** The switches, in Settings' order. */
    static final List<Kind> KINDS = List.of(
            new Kind(AppOpsManager.OPSTR_MANAGE_EXTERNAL_STORAGE,
                    AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE,
                    Manifest.permission.MANAGE_EXTERNAL_STORAGE, true, AppOpsManager.MODE_ERRORED),
            new Kind(AppOpsManager.OPSTR_SYSTEM_ALERT_WINDOW, AppOpsManager.OP_SYSTEM_ALERT_WINDOW,
                    Manifest.permission.SYSTEM_ALERT_WINDOW, false, AppOpsManager.MODE_ERRORED),
            new Kind(AppOpsManager.OPSTR_REQUEST_INSTALL_PACKAGES,
                    AppOpsManager.OP_REQUEST_INSTALL_PACKAGES,
                    Manifest.permission.REQUEST_INSTALL_PACKAGES, false, AppOpsManager.MODE_ERRORED),
            new Kind(AppOpsManager.OPSTR_WRITE_SETTINGS, AppOpsManager.OP_WRITE_SETTINGS,
                    Manifest.permission.WRITE_SETTINGS, false, AppOpsManager.MODE_ERRORED),
            new Kind(AppOpsManager.OPSTR_GET_USAGE_STATS, AppOpsManager.OP_GET_USAGE_STATS,
                    Manifest.permission.PACKAGE_USAGE_STATS, false, AppOpsManager.MODE_IGNORED),
            new Kind(AppOpsManager.OPSTR_MANAGE_MEDIA, AppOpsManager.OP_MANAGE_MEDIA,
                    Manifest.permission.MANAGE_MEDIA, true, AppOpsManager.MODE_ERRORED));

    /** A switch's state for one app: whether the app's own check reads "allowed". */
    record Entry(Kind kind, boolean allowed) {
        /** One row of the host protocol: the op's public name, a tab, allowed or denied. */
        String row() {
            return kind.opName + "\t" + (allowed ? "allowed" : "denied");
        }
    }

    static Kind kind(String opName) {
        if (opName != null) {
            for (Kind kind : KINDS) {
                if (kind.opName.equals(opName)) return kind;
            }
        }
        throw new IllegalArgumentException("Not a special access: " + opName);
    }

    private static ApkPackage installed(PackageManagerService pm, String packageName) {
        if (!SettingsIntents.validPackageName(packageName)) {
            throw new IllegalArgumentException("Invalid package name");
        }
        ApkPackage pkg = pm.get(packageName);
        if (pkg == null || pm.isRemovalPending(packageName)) {
            throw new IllegalArgumentException("Unknown package: " + packageName);
        }
        return pkg;
    }

    /** Whether Settings would show the switch: the app requests the permission. */
    static boolean declares(ApkPackage pkg, Kind kind) {
        if (!pkg.parsed.getRequestedPermissions().contains(kind.permission)) return false;
        // Install requests are an Android O capability (canRequestPackageInstalls).
        return kind.op != AppOpsManager.OP_REQUEST_INSTALL_PACKAGES
                || pkg.parsed.getTargetSdkVersion() >= android.os.Build.VERSION_CODES.O;
    }

    /** What the app's own check answers (Environment, Settings, PackageManager...). */
    private static boolean allowed(PackageManagerService pm, ApkPackage pkg, Kind kind) {
        if (kind.op == AppOpsManager.OP_REQUEST_INSTALL_PACKAGES) {
            return pm.canRequestPackageInstalls(pkg.packageName, 0);
        }
        int mode = pm.appOps().checkRawMode(kind.op, pkg.uid(), pkg.packageName);
        if (mode == AppOpsManager.MODE_DEFAULT) {
            return pm.permissions().checkPermission(pkg.packageName, kind.permission)
                    == PackageManager.PERMISSION_GRANTED;
        }
        return mode == AppOpsManager.MODE_ALLOWED;
    }

    /** The switches of an installed app; none for an app that requests none. */
    static List<Entry> query(PackageManagerService pm, String packageName) {
        ApkPackage pkg = installed(pm, packageName);
        List<Entry> entries = new ArrayList<>();
        for (Kind kind : KINDS) {
            if (declares(pkg, kind)) entries.add(new Entry(kind, allowed(pm, pkg, kind)));
        }
        return entries;
    }

    /**
     * The user turned a switch on or off: writes the mode as Settings does, and answers
     * the switch's state afterwards.
     */
    static Entry set(PackageManagerService pm, String packageName, String opName, boolean allow) {
        Kind kind = kind(opName);
        ApkPackage pkg = installed(pm, packageName);
        if (!declares(pkg, kind)) {
            throw new IllegalArgumentException(packageName + " does not request " + kind.permission);
        }
        int mode = allow ? AppOpsManager.MODE_ALLOWED : kind.deniedMode;
        AppOpsService ops = pm.appOps();
        if (kind.uidMode) {
            if (!ops.setUidModeIfCurrent(kind.op, pkg, mode)) {
                throw new IllegalStateException(packageName + " changed; try again");
            }
        } else {
            ops.setMode(kind.op, pkg.uid(), pkg.packageName, mode);
        }
        ApkPackage current = pm.get(packageName);
        if (current != pkg || pm.isRemovalPending(packageName)) {
            throw new IllegalStateException(packageName + " changed; try again");
        }
        return new Entry(kind, allowed(pm, pkg, kind));
    }

    static String[] rows(List<Entry> entries) {
        String[] rows = new String[entries.size()];
        for (int i = 0; i < rows.length; i++) rows[i] = entries.get(i).row();
        return rows;
    }
}
