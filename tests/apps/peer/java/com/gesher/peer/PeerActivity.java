/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.peer;

import android.app.Activity;
import android.app.Application;
import android.app.SearchManager;
import android.app.SearchableInfo;
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
        SearchManager search = getSystemService(SearchManager.class);
        ComponentName searchable = new ComponentName("com.gesher.components",
                "com.gesher.components.SecondActivity");
        SearchableInfo info = search.getSearchableInfo(searchable);
        if (info == null || !searchable.equals(info.getSearchActivity())
                || !"com.gesher.components.items".equals(info.getSuggestAuthority())) {
            throw new AssertionError("Manifest-visible foreign searchable metadata was hidden");
        }
        report("manifest-visible foreign searchable metadata verified");
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
