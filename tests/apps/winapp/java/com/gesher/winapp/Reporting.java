/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.winapp;

import android.app.Activity;
import android.content.res.Configuration;
import android.graphics.Point;
import android.graphics.Rect;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.WindowInsets;

/**
 * An Activity that reports on stdout what the test (tests/windows/tracks/51-windows3.ps1)
 * reads: its lifecycle callbacks ("Name onPause"), onTopResumedActivityChanged, and what it
 * sees of the display and its window ({@link #shape}): the configuration's orientation and
 * size, the display's rotation and size as the Activity's Display answers, the window
 * metrics, the orientation qualifier its resources resolve. A saved state carries a counter,
 * so a relaunched Activity shows it got it.
 */
public class Reporting extends Activity {
    private static final String TAG = "winapp";
    private int mSaves;

    static void say(String message) {
        System.out.println(message);
        System.out.flush();
        Log.i(TAG, message);
    }

    /** The center of {@code v} in its window: "x,y". */
    static String center(View v) {
        int[] at = new int[2];
        v.getLocationInWindow(at);
        return (at[0] + v.getWidth() / 2) + "," + (at[1] + v.getHeight() / 2);
    }

    String name() {
        return getClass().getSimpleName();
    }

    /**
     * "landscape, 800x400 dp, smallest 400 dp, rotation 90, display 800x400, real 800x400,
     * window 800x400, max 800x400, resources land".
     */
    String shape() {
        Configuration c = getResources().getConfiguration();
        StringBuilder s = new StringBuilder();
        s.append(c.orientation == Configuration.ORIENTATION_LANDSCAPE ? "landscape"
                : c.orientation == Configuration.ORIENTATION_PORTRAIT ? "portrait"
                : "orientation " + c.orientation);
        s.append(", ").append(c.screenWidthDp).append('x').append(c.screenHeightDp)
                .append(" dp, smallest ").append(c.smallestScreenWidthDp).append(" dp");
        try {
            Display d = getDisplay();
            Point size = new Point();
            Point real = new Point();
            getWindowManager().getDefaultDisplay().getSize(size);
            getWindowManager().getDefaultDisplay().getRealSize(real);
            s.append(", rotation ").append(d.getRotation() * 90).append(", display ")
                    .append(size.x).append('x').append(size.y).append(", real ").append(real.x)
                    .append('x').append(real.y);
            Rect w = getWindowManager().getCurrentWindowMetrics().getBounds();
            Rect m = getWindowManager().getMaximumWindowMetrics().getBounds();
            s.append(", window ").append(w.width()).append('x').append(w.height())
                    .append(", max ").append(m.width()).append('x').append(m.height());
        } catch (RuntimeException e) {
            s.append(", display: ").append(e);
        }
        s.append(", resources ").append(getString(R.string.orientation_name));
        return s.toString();
    }

    /** "ime visible true, bottom 300" of the window's insets now. */
    String imeInsets() {
        View decor = getWindow().getDecorView();
        WindowInsets insets = decor.getRootWindowInsets();
        if (insets == null) return "ime unknown";
        return "ime visible " + insets.isVisible(WindowInsets.Type.ime()) + ", bottom "
                + insets.getInsets(WindowInsets.Type.ime()).bottom;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null) mSaves = savedInstanceState.getInt("saves");
        say(name() + " onCreate" + (savedInstanceState != null
                ? " with saved state (saves " + mSaves + ")" : "") + ": " + shape());
    }

    @Override
    protected void onStart() {
        super.onStart();
        say(name() + " onStart");
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        say(name() + " onRestart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        say(name() + " onResume: " + shape());
    }

    @Override
    protected void onPause() {
        super.onPause();
        say(name() + " onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        say(name() + " onStop");
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putInt("saves", ++mSaves);
        say(name() + " onSaveInstanceState (saves " + mSaves + ")");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        say(name() + " onDestroy" + (isChangingConfigurations() ? " (changing configurations)"
                : isFinishing() ? " (finishing)" : ""));
    }

    @Override
    public void onTopResumedActivityChanged(boolean isTopResumedActivity) {
        super.onTopResumedActivityChanged(isTopResumedActivity);
        say(name() + " onTopResumedActivityChanged " + isTopResumedActivity
                + (isResumed() ? " (resumed)" : " (not resumed)"));
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        say(name() + " onConfigurationChanged: " + shape());
    }
}
