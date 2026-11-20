/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.IUriGrantsManager;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.net.Uri;
import android.os.IBinder;

/** Android URI persistence, ownership and privileged administration entry points. */
public final class UriGrantsManagerService extends IUriGrantsManager.Default {
    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private final UriGrants mGrants;

    public UriGrantsManagerService(PackageManagerService pm, ActivityTaskManagerService atm,
            ActivityManagerService am) {
        this(pm, atm, am != null ? am.uriGrants() : null);
    }

    public UriGrantsManagerService(PackageManagerService pm, ActivityTaskManagerService atm,
            UriGrants uriGrants) {
        mPm = pm;
        mCallers = new CallerIdentity(pm, atm);
        mGrants = uriGrants;
        if (mGrants != null) {
            mGrants.load();
        }
    }

    private void user(int userId) {
        if (userId != 0) throw new SecurityException("Only user 0 is available");
    }
    private void permission(String permission) {
        if (!mCallers.isSystem() && mPm.permissions().checkPermission(mCallers.callerPackage(), permission)
                != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("URI operation requires " + permission);
        }
    }
    private int callerUid() {
        int uid = mCallers.callerUid();
        if (android.os.UserHandle.isIsolated(uid)) {
            throw new SecurityException("Isolated callers cannot use URI grant administration");
        }
        if (!mCallers.isSystem()) mCallers.callerPackage();
        return uid;
    }
    private int persistenceUid(String packageName, int userId) {
        if (packageName == null) return callerUid();
        permission(android.Manifest.permission.FORCE_PERSISTABLE_URI_PERMISSIONS);
        return installedUid(packageName, userId);
    }
    private int installedUid(String packageName, int userId) {
        if (userId != 0) return -1;
        if ("android".equals(packageName)) return android.os.Process.SYSTEM_UID;
        if ("com.android.shell".equals(packageName)) return android.os.Process.SHELL_UID;
        synchronized (mPm) {
            if (mPm.isRemovalPending(packageName)) return -1;
            ApkPackage target = mPm.get(packageName);
            return target == null ? -1 : target.uid();
        }
    }
    @Override public void takePersistableUriPermission(Uri uri, int modes, String packageName, int userId) {
        int uid = persistenceUid(packageName, userId);
        user(userId);
        mGrants.takePersistable(uid, uri, modes, userId);
    }
    @Override public void releasePersistableUriPermission(Uri uri, int modes, String packageName, int userId) {
        int uid = persistenceUid(packageName, userId);
        user(userId);
        mGrants.releasePersistable(uid, uri, modes, userId, packageName == null);
    }
    @Override public void grantUriPermissionFromOwner(IBinder owner, int fromUid, String targetPackage,
            Uri uri, int modes, int sourceUserId, int targetUserId) {
        // AOSP resolves the owner before checking fromUid and nullable arguments.
        user(targetUserId);
        mGrants.grantFromOwner(owner, fromUid, targetPackage, uri, modes, sourceUserId, targetUserId);
    }
    @Override public ParceledListSlice getGrantedUriPermissions(String packageName, int userId) {
        permission(android.Manifest.permission.GET_APP_GRANTED_URI_PERMISSIONS); user(userId);
        return new ParceledListSlice(mGrants.granted(packageName));
    }
    @Override public void clearGrantedUriPermissions(String packageName, int userId) {
        permission(android.Manifest.permission.CLEAR_APP_GRANTED_URI_PERMISSIONS); user(userId);
        mGrants.clearGranted(packageName);
    }
    @Override public ParceledListSlice getUriPermissions(String packageName, boolean incoming,
            boolean persistedOnly) {
        int uid = callerUid();
        java.util.Objects.requireNonNull(packageName, "packageName");
        if (installedUid(packageName, 0) != uid) {
            throw new SecurityException("Package does not belong to the calling UID");
        }
        return new ParceledListSlice(mGrants.list(packageName, incoming, persistedOnly));
    }
    @Override public int checkGrantUriPermission_ignoreNonSystem(int sourceUid, String targetPackage,
            Uri uri, int modes, int userId) {
        if (!mCallers.isSystem() && mPm.permissions().checkUidPermission(mCallers.callerUid(),
                android.Manifest.permission.INTERACT_ACROSS_USERS_FULL, 0) != PackageManager.PERMISSION_GRANTED) return -1;
        user(userId);
        return mGrants.checkGrantUriPermission_ignoreNonSystem(sourceUid, targetPackage, uri, modes, userId);
    }
}
