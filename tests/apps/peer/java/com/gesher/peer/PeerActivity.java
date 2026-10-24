/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.peer;

import android.app.Activity;
import android.app.Application;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/**
 * Started by another package through {@code com.gesher.peer.SHOW}: reports which process it
 * runs in and who started it, then returns its pid as the result and finishes.
 */
public class PeerActivity extends Activity {
    static void report(String message) {
        System.out.println("peer: " + message);
        System.out.flush();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ComponentName caller = getCallingActivity();
        report("PeerActivity onCreate in process " + Application.getProcessName() + " (pid "
                + Process.myPid() + "), action " + getIntent().getAction() + ", started by "
                + (caller != null ? caller.flattenToShortString() : "no activity"));
    }

    @Override
    protected void onResume() {
        super.onResume();
        report("PeerActivity onResume, setResult(RESULT_OK, pid), finish()");
        setResult(RESULT_OK, new Intent().putExtra("pid", Process.myPid()));
        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        report("PeerActivity onDestroy");
    }
}
