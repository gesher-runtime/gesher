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
            a.performPause();
        }
        a.performStop(false /* preserveWindow */, "gesher-app");
        View decor = a.mDecor;
        a.performDestroy();
        if (decor != null && a.mWindowAdded) {
            a.getWindowManager().removeViewImmediate(decor);
        }
        a.mDecor = null;
        a.mWindowAdded = false;
    }
}
