/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.lifeapp;

import android.app.Activity;
import android.content.res.Configuration;
import android.os.Bundle;
import android.util.Log;
import android.view.View;

/**
 * An Activity that reports its lifecycle callbacks on stdout ("Name onPause"), as the test
 * (tests/windows/tracks/48-lifecycle.ps1) reads them: create (with the state it was given),
 * start, restart, resume, pause, stop, save state, destroy, window focus, configuration
 * changes. Each saved state carries a counter, so a relaunched Activity shows it got it.
 */
public class Reporting extends Activity {
    private static final String TAG = "lifeapp";
    private int mSaves;

    static void say(String message) {
        System.out.println(message);
        System.out.flush();
        Log.i(TAG, message);
    }

    /** The center of {@code v} in the display's coordinates: "x,y". */
    static String center(View v) {
        int[] at = new int[2];
        v.getLocationOnScreen(at);
        return (at[0] + v.getWidth() / 2) + "," + (at[1] + v.getHeight() / 2);
    }

    /** "WxH dp, orientation, layout size" of a configuration. */
    static String describe(Configuration c) {
        String size;
        switch (c.screenLayout & Configuration.SCREENLAYOUT_SIZE_MASK) {
            case Configuration.SCREENLAYOUT_SIZE_SMALL: size = "small"; break;
            case Configuration.SCREENLAYOUT_SIZE_NORMAL: size = "normal"; break;
            case Configuration.SCREENLAYOUT_SIZE_LARGE: size = "large"; break;
            case Configuration.SCREENLAYOUT_SIZE_XLARGE: size = "xlarge"; break;
            default: size = "undefined"; break;
        }
        return c.screenWidthDp + "x" + c.screenHeightDp + " dp, smallest " + c.smallestScreenWidthDp
                + " dp, " + (c.orientation == Configuration.ORIENTATION_LANDSCAPE ? "landscape"
                        : c.orientation == Configuration.ORIENTATION_PORTRAIT ? "portrait"
                        : "orientation " + c.orientation) + ", " + size;
    }

    String name() {
        return getClass().getSimpleName();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null) mSaves = savedInstanceState.getInt("saves");
        say(name() + " onCreate" + (savedInstanceState != null
                ? " with saved state (saves " + mSaves + ")" : "") + ", configuration "
                + describe(getResources().getConfiguration()) + ", " + qualifiers());
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
        say(name() + " onResume");
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
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        say(name() + " onWindowFocusChanged " + hasFocus + (isResumed() ? " (resumed)"
                : " (not resumed)"));
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        say(name() + " onConfigurationChanged: " + describe(newConfig) + ", " + qualifiers());
    }

    /** Which of the app's resource qualifiers its resources resolve to now. */
    String qualifiers() {
        return "resources " + getString(R.string.orientation_name) + " "
                + getString(R.string.width_name);
    }
}
