/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.app;

import android.view.View;
import android.view.Window;
import android.view.WindowManager;

/**
 * An Activity's window, for gesher-app's single-process driver (GesherAppDriver drives the
 * lifecycle without ActivityThread's transactions): what handleResumeActivity does to add
 * the decor view to the window manager and make it visible, and what the destroy
 * transaction does when the window is closed. In {@code android.app} for Activity's
 * package-private state (mDecor, mWindowAdded, makeVisible, perform*).
 */
public final class GesherActivityWindow {
    private GesherActivityWindow() {
    }

    /**
     * handleResumeActivity's window part, for a resumed Activity: its decor view goes to
     * the window manager as a TYPE_BASE_APPLICATION window (ViewRootImpl, then the window
     * session: com.gesher.app.window.WindowSession), then becomes visible.
     */
    public static void makeVisible(Activity a) {
        if (a.mFinished || a.mDecor != null) {
            return;
        }
        Window window = a.getWindow();
        View decor = window.getDecorView();
        decor.setVisibility(View.INVISIBLE);
        WindowManager.LayoutParams l = window.getAttributes();
        a.mDecor = decor;
        l.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
        if (a.mVisibleFromClient && !a.mWindowAdded) {
            a.mWindowAdded = true;
            a.getWindowManager().addView(decor, l);
        }
        a.mVisibleFromServer = true;
        if (a.mVisibleFromClient) {
            a.makeVisible();
        }
    }

    /**
     * The Activity goes from resumed to stopped as ActivityThread takes it there when its
     * task is no longer visible (its window minimized; docs/lifecycle.md): onPause, onStop,
     * then onSaveInstanceState (targetSdk 28 and up; before onStop below), and its decor
     * view is made invisible (updateVisibility). Returns the saved state, or null if it was
     * not resumed or started.
     */
    public static android.os.Bundle stop(Activity a) {
        if (a.mFinished || a.mStopped) return null;
        if (a.isResumed()) {
            // No longer the top resumed Activity (ActivityThread reports it as it pauses).
            a.performTopResumedActivityChanged(false, "pausing");
            a.performPause();
            GesherAppDriver.activityPaused();
        }
        android.os.Bundle state = new android.os.Bundle();
        boolean before = a.getApplicationInfo().targetSdkVersion < android.os.Build.VERSION_CODES.P;
        // Instrumentation.callActivityOnSaveInstanceState's call.
        if (before) a.performSaveInstanceState(state);
        a.performStop(false /* preserveWindow */, "gesher-app");
        GesherAppDriver.activityStopped();
        if (!before) a.performSaveInstanceState(state);
        View decor = a.mDecor;
        if (decor != null && a.mVisibleFromServer) {
            a.mVisibleFromServer = false;
            decor.setVisibility(View.INVISIBLE);
        }
        return state;
    }

    /**
     * A stopped Activity comes back to resumed (its window restored): onRestart, onStart,
     * onResume, its decor view visible again.
     */
    public static void restart(Activity a) {
        if (a.mFinished || !a.mStopped) return;
        a.performRestart(true /* start */);
        if (a.mDecor != null && !a.mVisibleFromServer) {
            a.mVisibleFromServer = true;
            if (a.mVisibleFromClient) a.makeVisible();
        }
        GesherAppDriver.activityResuming();
        a.performResume(false /* followedByPause */, "gesher-app");
        GesherAppDriver.activityResumed();
        a.performTopResumedActivityChanged(true, "topWhenResuming");
    }

    /**
     * Finishes the Activity as the server would once its window is closed: finish(), then
     * pause, stop and destroy, and its window is removed (the Win32 window closes).
     */
    public static void finishAndDestroy(Activity a) {
        if (!a.mFinished) {
            a.finish();
            // finish() asks the activity client controller; the stand-in does not finish.
            a.mFinished = true;
        }
        if (a.isResumed()) {
            a.performTopResumedActivityChanged(false, "pausing");
            a.performPause();
            GesherAppDriver.activityPaused();
        }
        a.performStop(false /* preserveWindow */, "gesher-app");
        GesherAppDriver.activityStopped();
        View decor = a.mDecor;
        a.performDestroy();
        if (decor != null && a.mWindowAdded) {
            a.getWindowManager().removeViewImmediate(decor);
        }
        a.mDecor = null;
        a.mWindowAdded = false;
    }

    /** This process's Activity the camera follows: the last one created or resumed. */
    private static volatile java.lang.ref.WeakReference<Activity> sCurrent =
            new java.lang.ref.WeakReference<>(null);
    private static final java.util.concurrent.atomic.AtomicBoolean sTracking =
            new java.util.concurrent.atomic.AtomicBoolean();
    /**
     * A seed found nothing or timed out: not retried until an Activity lifecycle callback
     * or the callbacks' registration, so callers do not each wait for the main thread.
     */
    private static volatile boolean sSeedFailed;

    /**
     * The {@code Surface.ROTATION_*} display rotation of this process's current Activity
     * (the one being created, else the last resumed), as its own {@code Display.getRotation()}
     * reports it, or {@link WindowConfiguration#ROTATION_UNDEFINED} when the process has none
     * (a service, or before its Application exists). Any thread: the Activity is chosen on
     * the main thread by the lifecycle callbacks; reading its Configuration is a field read.
     */
    public static int activeDisplayRotation() {
        trackActivities();
        Activity a = sCurrent.get();
        if (a == null && !sSeedFailed) a = seedFromMainThread();
        if (a == null || a.isDestroyed()) return WindowConfiguration.ROTATION_UNDEFINED;
        try {
            return a.getResources().getConfiguration().windowConfiguration.getDisplayRotation();
        } catch (RuntimeException e) {
            return WindowConfiguration.ROTATION_UNDEFINED;
        }
    }

    /**
     * Follows Activities from their creation (onActivityPreCreated runs after attach, so
     * the new Activity's configuration is set, before onCreate: the record of the Activity
     * being replaced is not used) and on each resume. Registered once, when the
     * Application exists: by the app process before its first Activity transaction
     * (GesherAppProcess, GesherAppDriver), else on the first rotation query.
     */
    static void trackActivities() {
        if (sTracking.get()) return;
        Application app = ActivityThread.currentApplication();
        if (app == null || !sTracking.compareAndSet(false, true)) return;
        sSeedFailed = false;
        app.registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
            @Override public void onActivityPreCreated(Activity a, android.os.Bundle b) {
                sCurrent = new java.lang.ref.WeakReference<>(a);
                sSeedFailed = false;
            }
            @Override public void onActivityResumed(Activity a) {
                sCurrent = new java.lang.ref.WeakReference<>(a);
                sSeedFailed = false;
            }
            @Override public void onActivityDestroyed(Activity a) {
                if (sCurrent.get() == a) sCurrent = new java.lang.ref.WeakReference<>(null);
                sSeedFailed = false;
            }
            @Override public void onActivityCreated(Activity a, android.os.Bundle b) {}
            @Override public void onActivityStarted(Activity a) {}
            @Override public void onActivityPaused(Activity a) {}
            @Override public void onActivityStopped(Activity a) {}
            @Override public void onActivitySaveInstanceState(Activity a, android.os.Bundle b) {}
        });
    }

    /**
     * Before the callbacks saw an Activity (they were registered after it was created): the
     * top resumed one, else a resumed one, else one being created or started, read from
     * ActivityThread's records on the main thread, which owns them. Waits for the main
     * thread up to a second (an Activity's onCreate may hold it), once until the next
     * lifecycle callback; null if it has none.
     */
    private static Activity seedFromMainThread() {
        ActivityThread at = ActivityThread.currentActivityThread();
        if (at == null) return null;
        java.util.concurrent.Callable<Activity> find = () -> {
            Activity resumed = null;
            Activity starting = null;
            for (int i = 0; i < at.mActivities.size(); i++) {
                ActivityThread.ActivityClientRecord r = at.mActivities.valueAt(i);
                if (r == null || r.activity == null || r.activity.mFinished) continue;
                int state = r.getLifecycleState();
                if (state == android.app.servertransaction.ActivityLifecycleItem.ON_RESUME) {
                    if (r.isTopResumedActivity) return r.activity;
                    if (resumed == null) resumed = r.activity;
                } else if (state == android.app.servertransaction.ActivityLifecycleItem.ON_CREATE
                        || state == android.app.servertransaction.ActivityLifecycleItem.ON_START) {
                    if (starting == null) starting = r.activity;
                }
            }
            return resumed != null ? resumed : starting;
        };
        Activity found;
        try {
            if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) {
                found = find.call();
            } else {
                java.util.concurrent.FutureTask<Activity> task =
                        new java.util.concurrent.FutureTask<>(find);
                if (!new android.os.Handler(android.os.Looper.getMainLooper()).post(task)) {
                    sSeedFailed = true;
                    return null;
                }
                found = task.get(1000, java.util.concurrent.TimeUnit.MILLISECONDS);
            }
        } catch (Exception e) {
            sSeedFailed = true;
            return null;
        }
        if (found == null) sSeedFailed = true;
        if (found != null && sCurrent.get() == null) {
            sCurrent = new java.lang.ref.WeakReference<>(found);
        }
        return found;
    }
}
