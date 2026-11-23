/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.StatusBarManager;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.graphics.drawable.Icon;
import android.os.RemoteException;
import android.service.quicksettings.TileService;

import com.android.internal.statusbar.IAddTileResultCallback;
import com.android.internal.statusbar.IStatusBarService;

import java.util.Objects;

/**
 * {@code statusbar}: StatusBarManagerService with no SystemUI status bar registered,
 * which is what a Windows desktop has (no Quick Settings panel). Without the service
 * {@code StatusBarManager} dereferences null: TileService.requestListeningState, a public
 * API, threw NullPointerException in Opera after its welcome screens.
 *
 * <p>The public tile APIs answer as the platform service does while {@code mBar} is null
 * (frameworks/base StatusBarManagerService): the caller must own the tile's package
 * (SecurityException otherwise); a listening-state request then has nothing to notify,
 * and an add-tile request reports {@link StatusBarManager#TILE_ADD_REQUEST_ERROR_NO_STATUS_BAR_SERVICE}
 * after the same user, component and foreground checks. The other methods are the
 * status bar's own (signature permissions) and keep {@link StandIn}'s answers.
 */
final class StatusBarService extends IStatusBarService.Default {
    private final PackageManagerService mPm;
    private final ActivityTaskManagerService mAtm;
    private final CallerIdentity mCallers;

    StatusBarService(PackageManagerService pm, ActivityTaskManagerService atm) {
        mPm = pm;
        mAtm = atm;
        mCallers = new CallerIdentity(pm, atm);
    }

    /** checkCallingUidPackage: the caller's app id must be the package's. */
    private void requireOwner(String packageName) {
        if (mCallers.isSystem()) return;
        String caller = mCallers.callerPackage();
        ApkPackage callerPackage = mPm.get(caller);
        ApkPackage owner = packageName == null ? null : mPm.get(packageName);
        if (callerPackage == null || owner == null || callerPackage.uid() != owner.uid()) {
            throw new SecurityException("Package " + packageName
                    + " does not belong to the calling uid " + mCallers.callerUid());
        }
    }

    @Override
    public void requestTileServiceListeningState(ComponentName componentName, int userId) {
        Objects.requireNonNull(componentName, "componentName");
        requireOwner(componentName.getPackageName());
        // Another user: a no-op (REQUEST_LISTENING_OTHER_USER_NOOP). This user: no
        // status bar to ask the tile to listen.
    }

    @Override
    public void requestAddTile(ComponentName componentName, CharSequence label, Icon icon,
            int userId, IAddTileResultCallback callback) {
        Objects.requireNonNull(componentName, "componentName");
        Objects.requireNonNull(callback, "callback");
        requireOwner(componentName.getPackageName());
        int result;
        if (userId != 0) {
            result = StatusBarManager.TILE_ADD_REQUEST_ERROR_NOT_CURRENT_USER;
        } else if (!isTileService(componentName)) {
            result = StatusBarManager.TILE_ADD_REQUEST_ERROR_BAD_COMPONENT;
        } else if (!mCallers.isSystem() && mAtm.topResumedUid() != mCallers.callerUid()) {
            result = StatusBarManager.TILE_ADD_REQUEST_ERROR_APP_NOT_IN_FOREGROUND;
        } else {
            result = StatusBarManager.TILE_ADD_REQUEST_ERROR_NO_STATUS_BAR_SERVICE;
        }
        try {
            callback.onTileRequest(result);
        } catch (RemoteException e) {
            System.err.println("statusbar: requestAddTile callback failed: " + e);
        }
    }

    @Override
    public void cancelRequestAddTile(String packageName) {
        requireOwner(packageName);
        // No request is ever pending without a status bar.
    }

    /** isComponentValidTileService: an exported service handling ACTION_QS_TILE. */
    private boolean isTileService(ComponentName componentName) {
        Intent intent = new Intent(TileService.ACTION_QS_TILE).setComponent(componentName);
        ResolveInfo resolved = mPm.resolveService(intent, null, 0, 0);
        return resolved != null && resolved.serviceInfo != null && resolved.serviceInfo.exported;
    }
}
