/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.HandoffActivityData;
import android.app.IActivityClientController;
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
        if (r != null) mAtm.activityStopped(r);
    }

    @Override
    public void activityDestroyed(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) mAtm.activityDestroyed(r);
    }

    @Override
    public void reportSizeConfigurations(IBinder token,
            SizeConfigurationBuckets sizeConfigurations) {
        // Nothing resizes.
    }

    @Override
    public boolean finishActivity(IBinder token, int code, Intent data, int finishTask) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null && mAtm.finish(r, "app-request");
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
}
