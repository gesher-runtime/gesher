/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.AppOpsManager;
import android.content.pm.PackageManager;

import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import java.util.function.IntSupplier;

/** AppOps delivery accounting for a broker-authenticated camera session. */
final class ProtectedCameraPolicy {
    private final PackageManagerService mPm;
    // Only the private capture server thread reads and writes these sessions.
    private final Map<Long, AppOpsService.CameraDelivery> mDeliveries = new HashMap<>();

    ProtectedCameraPolicy(PackageManagerService pm) {
        mPm = pm;
    }

    boolean installed(int uid, int kind, long high, long low, long session, String tag,
            ActivityManagerService am) {
        if (kind != Zygote.CapturePolicy.CAMERA_START) return existing(kind, session);
        String installation = new UUID(high, low).toString();
        for (ApkPackage pkg : mPm.packages()) {
            if (pkg.uid() != uid || !installation.equals(pkg.enabledStateOwner())) continue;
            return start(session, pkg, tag, () -> {
                if (mPm.get(pkg.packageName) != pkg || mPm.isRemovalPending(pkg.packageName)
                        || mPm.checkUidPermission(Manifest.permission.CAMERA, uid)
                                != PackageManager.PERMISSION_GRANTED) return -1;
                int mode = mPm.appOps().checkRawMode(AppOpsManager.OP_CAMERA, uid, pkg.packageName);
                return permitsForeground(mode) ? am.cameraForegroundState(uid) : -1;
            });
        }
        return false;
    }

    boolean scoped(int kind, long session, PermissionPolicy.ScopedOwner owner, String tag,
            IntSupplier foregroundState) {
        if (kind != Zygote.CapturePolicy.CAMERA_START) return existing(kind, session);
        return start(session, owner.code(), tag, () -> owner.isCurrent()
                && owner.check(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                && permitsForeground(owner.mode(Manifest.permission.CAMERA))
                        ? foregroundState.getAsInt() : -1);
    }

    private static boolean permitsForeground(int mode) {
        return mode == AppOpsManager.MODE_ALLOWED || mode == AppOpsManager.MODE_FOREGROUND;
    }

    private boolean start(long session, ApkPackage owner, String tag, IntSupplier policy) {
        if (session == 0 || mDeliveries.containsKey(session)) return false;
        AppOpsService.CameraDelivery delivery = mPm.appOps().startCameraDelivery(owner, tag, policy);
        if (delivery == null) return false;
        mDeliveries.put(session, delivery);
        return true;
    }

    private boolean existing(int kind, long session) {
        if (kind == Zygote.CapturePolicy.CAMERA_RESET) {
            for (AppOpsService.CameraDelivery delivery : mDeliveries.values()) delivery.close();
            mDeliveries.clear();
            return true;
        }
        AppOpsService.CameraDelivery delivery = mDeliveries.get(session);
        if (delivery == null) return false;
        if (kind == Zygote.CapturePolicy.CAMERA_CHECK && delivery.check()) return true;
        mDeliveries.remove(session);
        delivery.close();
        return kind == Zygote.CapturePolicy.CAMERA_FINISH;
    }
}
