/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.HandoffActivityData;
import android.app.IActivityClientController;
import android.app.IRequestFinishCallback;
import android.content.ComponentName;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PersistableBundle;
import android.os.RemoteException;
import android.view.Display;
import android.window.SizeConfigurationBuckets;

/**
 * ActivityClientController: what an Activity reports to, and asks of, the activity task
 * manager about itself ({@link ActivityTaskManagerService}). Tokens the server did not
 * hand out (gesher-app's single-process driver makes its own) get the answers a lone
 * activity on the default display would.
 *
 * <p>One report is Gesher's own ({@link StandIn.Extension}): an app process draws its
 * windows itself (ADR 0029), so the first frame of an Activity's window, which
 * WindowManagerService sees in Android (ActivityRecord.onWindowsDrawn), reaches the server
 * from the process's window session (com.gesher.app.window.ActivityClientHook#reportWindowsDrawn).
 */
public final class ActivityClientController extends IActivityClientController.Default
        implements StandIn.Extension {
    private final ActivityTaskManagerService mAtm;

    ActivityClientController(ActivityTaskManagerService atm) {
        mAtm = atm;
    }

    @Override
    public boolean onExtensionTransaction(int code, android.os.Parcel data,
            android.os.Parcel reply, int flags) throws RemoteException {
        if (code != com.gesher.app.window.ActivityClientHook.WINDOWS_DRAWN) return false;
        data.enforceInterface(com.gesher.app.window.ActivityClientHook.WINDOWS_DRAWN_DESCRIPTOR);
        IBinder token = data.readStrongBinder();
        data.enforceNoDataAvail();
        windowsDrawn(token);
        if (reply != null) reply.writeNoException();
        return true;
    }

    /** The first frame of the Activity's window was drawn, in its own process. */
    void windowsDrawn(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        // The owner is checked again under the lock with the caller's pid: a report that
        // was queued by a process which then died must not mark the record once it is
        // restarted in a new process.
        if (r != null && mAtm.requireActivityOwner(r)) {
            mAtm.activityWindowsDrawn(r, android.os.Binder.getCallingPid());
        }
    }

    @Override
    public void activityIdle(IBinder token, Configuration config, boolean stopProfiling) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) mAtm.activityIdle(r);
    }

    @Override
    public void activityResumed(IBinder token, boolean handleSplashScreenExit) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null && mAtm.requireActivityOwner(r)) mAtm.activityResumed(r);
    }

    /** After TopResumedActivityChangeItem(false) (multi-resume, windows3 track). */
    @Override
    public void activityTopResumedStateLost(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) SystemServer.log("activityTopResumedStateLost: " + r.name());
    }

    @Override
    public void activityPaused(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null && mAtm.requireActivityOwner(r)) mAtm.activityPaused(r);
    }

    @Override
    public void activityStopped(IBinder token, Bundle state, PersistableBundle persistentState,
            HandoffActivityData handoffActivityData, CharSequence description) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null && mAtm.requireActivityOwner(r)) mAtm.activityStopped(r, state, persistentState);
    }

    @Override
    public void activityDestroyed(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null && mAtm.requireActivityOwner(r)) mAtm.activityDestroyed(r);
    }

    /** After an ActivityRelaunchItem (locale track): the new instance reports its states. */
    @Override
    public void activityRelaunched(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) SystemServer.log("activityRelaunched: " + r.name());
    }

    /** Activity.recreate(): the app relaunches it by itself (a local transaction). */
    @Override
    public void activityLocalRelaunch(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) SystemServer.log("activityLocalRelaunch: " + r.name());
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
        // An already removed managed Activity is already finished. Keep the
        // standalone driver fallback below for tokens without a managed record.
        if (r != null && !mAtm.requireActivityOwner(r)) return true;
        if (r != null) return mAtm.finishFromApp(r, code, data, finishTask)
                || mAtm.isActivityRetired(r);
        com.gesher.app.window.WindowSession session = com.gesher.app.window.WindowSession.get();
        return session == null || session.finishLocalActivity(token);
    }

    @Override
    public boolean finishActivityAffinity(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null && mAtm.requireActivityOwner(r) && mAtm.finishAffinity(r);
    }

    @Override
    public void finishSubActivity(IBinder token, String resultWho, int requestCode) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null && mAtm.requireActivityOwner(r)) {
            mAtm.finishSubActivities(r, resultWho, requestCode);
        }
    }

    @Override
    public boolean navigateUpTo(IBinder token, Intent destIntent, String resolvedType,
            int resultCode, Intent resultData) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null && mAtm.requireActivityOwner(r)
                && mAtm.navigateUpTo(r, destIntent, resolvedType, resultCode,
                resultData);
    }

    @Override
    public int getDisplayId(IBinder activityToken) {
        return Display.DEFAULT_DISPLAY;
    }

    @Override
    public int getTaskForActivity(IBinder token, boolean onlyRoot) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r == null) return 1;  // gesher-app's single-process activity: task 1
        // onlyRoot (Activity.isTaskRoot): -1 unless it is its task's root.
        return !onlyRoot || mAtm.isTaskRoot(r) ? r.taskId : -1;
    }

    /**
     * Activity.onBackPressed with no fragment to pop (onBackInvoked; so OnBackPressedDispatcher's
     * fallback, and a dialog's "discard changes" that goes back): as Android, the activity
     * finishes (callback.requestFinish) unless it is the last activity of a task the home
     * screen launched with its launcher intent, whose task moves to the back (minimized).
     * A root started otherwise (a mail compose screen opened from a link or another task)
     * finishes, and its task ends.
     */
    @Override
    public void onBackPressed(IBinder token, IRequestFinishCallback callback) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null && mAtm.moveTaskToBackOnBack(r)) return;
        try {
            callback.requestFinish();
        } catch (RemoteException e) {
            // The app is gone.
        }
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

    /**
     * Activity.setRequestedOrientation (windows3 track): the task's display turns for its top
     * activity's orientation (ActivityTaskManagerService.setRequestedOrientation).
     */
    @Override
    public void setRequestedOrientation(IBinder token, int requestedOrientation) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r != null) mAtm.setRequestedOrientation(r, requestedOrientation);
    }

    @Override
    public int getRequestedOrientation(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        return r != null ? mAtm.getRequestedOrientation(r)
                : android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
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
        ActivityTaskManagerService.ActivityRecord result = r == null ? null : r.resultTo;
        return result != null ? result.component() : null;
    }

    @Override
    public String getCallingPackage(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        ActivityTaskManagerService.ActivityRecord result = r == null ? null : r.resultTo;
        return result != null ? result.info.packageName : null;
    }

    @Override
    public String getLaunchedFromPackage(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r == null || !mAtm.requireActivityOwner(r)) return null;
        return r.launchedFromPackage;
    }

    @Override
    public int getLaunchedFromUid(IBinder token) {
        ActivityTaskManagerService.ActivityRecord r = mAtm.forToken(token);
        if (r == null || !mAtm.requireActivityOwner(r)) return android.os.Process.INVALID_UID;
        ActivityCallerState.Snapshot caller = mAtm.callerSnapshot(token, null, true);
        return caller != null ? caller.uid : android.os.Process.INVALID_UID;
    }

    @Override
    public int getActivityCallerUid(IBinder activityToken, IBinder callerToken) {
        ActivityCallerState.Snapshot caller = mAtm.visibleCaller(activityToken, callerToken, false);
        return caller == null ? android.os.Process.INVALID_UID : caller.uid;
    }

    @Override
    public String getActivityCallerPackage(IBinder activityToken, IBinder callerToken) {
        ActivityCallerState.Snapshot caller = mAtm.visibleCaller(activityToken, callerToken, false);
        return caller == null ? null : caller.packageName;
    }

    @Override
    public int checkActivityCallerContentUriPermission(IBinder activityToken, IBinder callerToken,
            android.net.Uri uri, int modeFlags, int userId) {
        return mAtm.checkActivityCallerContentUriPermission(activityToken, callerToken, uri,
                modeFlags, userId);
    }
}
