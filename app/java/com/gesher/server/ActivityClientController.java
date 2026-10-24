/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.HandoffActivityData;
import android.app.IActivityClientController;
import android.content.ComponentName;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PersistableBundle;
import android.view.Display;
import android.window.SizeConfigurationBuckets;

/**
 * ActivityClientController: what an Activity reports to, and asks of, the activity task
 * manager about itself ({@link ActivityTaskManagerService}). Tokens the server did not
 * hand out (gesher-app's single-process driver makes its own) get the answers a lone
 * activity on the default display would.
 */
public final class ActivityClientController extends IActivityClientController.Default {
    private final ActivityTaskManagerService mAtm;

    ActivityClientController(ActivityTaskManagerService atm) {
        mAtm = atm;
    }

    @Override
    public void activityIdle(IBinder token, Configuration config, boolean stopProfiling) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) mAtm.activityIdle(r);
    }

    @Override
    public void activityResumed(IBinder token, boolean handleSplashScreenExit) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) mAtm.activityResumed(r);
    }

    @Override
    public void activityTopResumedStateLost(IBinder token) {
    }

    @Override
    public void activityPaused(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) mAtm.activityPaused(r);
    }

    @Override
    public void activityStopped(IBinder token, Bundle state, PersistableBundle persistentState,
            HandoffActivityData handoffActivityData, CharSequence description) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) mAtm.activityStopped(r, state);
    }

    @Override
    public void activityDestroyed(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) mAtm.activityDestroyed(r);
    }

    @Override
    public void reportSizeConfigurations(IBinder token,
            SizeConfigurationBuckets sizeConfigurations) {
        // Which size changes count when its task is resized (resizeTask).
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) mAtm.reportSizeConfigurations(r, sizeConfigurations);
    }

    @Override
    public boolean finishActivity(IBinder token, int code, Intent data, int finishTask) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null && mAtm.finish(r, "app-request", code, data);
    }

    @Override
    public boolean navigateUpTo(IBinder token, Intent destIntent, String resolvedType,
            int resultCode, Intent resultData) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null && mAtm.navigateUpTo(r, destIntent, resolvedType, resultCode,
                resultData);
    }

    @Override
    public int getDisplayId(IBinder activityToken) {
        return Display.DEFAULT_DISPLAY;
    }

    @Override
    public int getTaskForActivity(IBinder token, boolean onlyRoot) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null ? r.taskId : 1;  // gesher-app's single-process activity: task 1
    }

    @Override
    public void setTaskDescription(IBinder token, ActivityManager.TaskDescription values) {
        // No recents screen to show it in.
    }

    @Override
    public boolean isTopOfTask(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r == null || mAtm.isTopOfTask(r);
    }

    @Override
    public boolean willActivityBeVisible(IBinder token) {
        // Visible unless an opaque activity above it in its task covers it, or its task is
        // at the back (docs/lifecycle.md).
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r == null || mAtm.willBeVisible(r);
    }

    @Override
    public boolean moveActivityTaskToBack(IBinder token, boolean nonRoot) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null && mAtm.moveTaskToBack(r, nonRoot);
    }

    @Override
    public boolean convertToTranslucent(IBinder token, Bundle options) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null && mAtm.setTranslucent(r, true);
    }

    @Override
    public boolean convertFromTranslucent(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null && mAtm.setTranslucent(r, false);
    }

    @Override
    public ComponentName getCallingActivity(IBinder token) {
        // Who will get this activity's result (startActivityForResult), if anyone.
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null && r.resultTo != null ? r.resultTo.component() : null;
    }

    @Override
    public String getCallingPackage(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null && r.resultTo != null ? r.resultTo.info.packageName : null;
    }

    @Override
    public String getLaunchedFromPackage(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null ? r.launchedFromPackage : null;
    }

    @Override
    public int getLaunchedFromUid(IBinder token) {
        return android.os.Process.myUid();  // every gesher process has the same uid
    }
}
