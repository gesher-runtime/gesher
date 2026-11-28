// SPDX-License-Identifier: Apache-2.0
package com.gesher.launchreset;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.TextView;

public final class LaunchRoot extends Activity {
    private static final String TAG = "F01LauncherReset";
    private static int rootCreates;
    private int rootResumes;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView view = new TextView(this);
        view.setText("Gesher Reset Probe");
        setContentView(view);
        Log.i(TAG, "ROOT_CREATE count=" + ++rootCreates + " task=" + getTaskId()
                + " flags=" + launchFlags(getIntent()));
    }

    @Override protected void onResume() {
        super.onResume();
        Log.i(TAG, "ROOT_RESUME count=" + ++rootResumes + " task=" + getTaskId());
        if (rootResumes == 1) {
            new Handler(Looper.getMainLooper()).postDelayed(() ->
                    startActivity(new Intent(this, Child.class)), 350);
        }
    }

    public static final class Child extends Activity {
        @Override protected void onCreate(Bundle state) {
            super.onCreate(state);
            TextView view = new TextView(this);
            view.setText("Child");
            setContentView(view);
        }

        @Override protected void onResume() {
            super.onResume();
            Log.i(TAG, "CHILD_RESUME task=" + getTaskId());
        }

        @Override protected void onDestroy() {
            super.onDestroy();
            Log.i(TAG, "CHILD_DESTROY task=" + getTaskId() + " finishing=" + isFinishing());
        }
    }

    public static final class Requested extends Activity {
        @Override protected void onCreate(Bundle state) {
            super.onCreate(state);
            TextView view = new TextView(this);
            view.setText("Requested");
            setContentView(view);
            Log.i(TAG, "REQUESTED action=" + getIntent().getAction() + " flags="
                    + launchFlags(getIntent()) + " data=" + getIntent().getDataString());
        }
    }

    private static String launchFlags(Intent intent) {
        int mask = Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED;
        return Integer.toHexString(intent.getFlags() & mask);
    }
}
