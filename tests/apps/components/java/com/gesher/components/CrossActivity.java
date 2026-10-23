/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.app.ActivityManager;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;

import java.util.List;

/**
 * Another package's components (tests/apps/peer, com.gesher.peer), each in that package's
 * process, which the system server starts for them: an activity started through an
 * implicit intent, for a result; then, once that process ended with the activity, its
 * content provider (insert, getType, call). Then it finishes. Started by the launcher with
 * {@code --activity .CrossActivity}.
 */
public class CrossActivity extends ReportingActivity {
    static final String ACTION_PEER = "com.gesher.peer.SHOW";
    static final Uri NOTES = Uri.parse("content://com.gesher.peer.notes/notes");
    private static final int REQUEST = 2;
    private static final String PEER = "com.gesher.peer";
    private final Handler mHandler = new Handler(Looper.getMainLooper());

    @Override
    protected void onResume() {
        super.onResume();
        if (mResumes == 1) {
            Report.line("CrossActivity (pid " + Process.myPid() + ") starts " + ACTION_PEER
                    + " (implicit, another package) for a result");
            startActivityForResult(new Intent(ACTION_PEER), REQUEST);
        } else if (mResumes == 2) {
            // Once the peer's process is gone (it ends with its activity), its provider:
            // the system server starts the process again for it.
            mHandler.post(this::whenPeerGone);
        }
    }

    private int mChecks;

    private void whenPeerGone() {
        boolean running = false;
        ActivityManager am = getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> processes = am.getRunningAppProcesses();
        if (processes != null) {
            for (ActivityManager.RunningAppProcessInfo p : processes) {
                running |= PEER.equals(p.processName);
            }
        }
        if (running && ++mChecks < 150) {
            mHandler.postDelayed(this::whenPeerGone, 200);
            return;
        }
        Report.line(running ? "the peer's process still runs" : "the peer's process ended");
        try {
            provider();
        } catch (Throwable t) {
            Report.line("FAILED: the peer's provider: " + t);
            t.printStackTrace(System.out);
        }
        Report.line("cross-package steps ran");
        finish();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        int pid = data != null ? data.getIntExtra("pid", -1) : -1;
        Report.line("CrossActivity onActivityResult: request " + requestCode + ", result "
                + (resultCode == RESULT_OK ? "RESULT_OK" : String.valueOf(resultCode))
                + ", the peer's pid " + pid + ", another process: "
                + (pid > 0 && pid != Process.myPid()));
    }

    private void provider() {
        ContentResolver cr = getContentResolver();
        ContentValues values = new ContentValues();
        values.put("text", "hello from components");
        Report.line("peer provider insert: " + cr.insert(NOTES, values));
        Report.line("peer provider getType: " + cr.getType(NOTES));
        Bundle b = cr.call(NOTES, "describe", null, null);
        int pid = b != null ? b.getInt("pid", -1) : -1;
        Report.line("peer provider call describe: notes [" + (b != null ? b.getString("notes")
                : null) + "], in another process: " + (pid > 0 && pid != Process.myPid()));
    }
}
