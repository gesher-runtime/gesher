/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.ContentValues;
import android.database.ContentObserver;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PersistableBundle;

import java.util.Collection;

/**
 * Content URI triggers (hwsvc2, ADR 0051): a job (TestJobService, "content") triggered by
 * changes under NotesProvider's items, a ContentObserver on the provider, and two inserts
 * (each notifies its item's URI); the job runs once with both URIs, the observer is told
 * of each. Finishes after 10 s.
 */
public class ContentActivity extends Activity {
    static final int CONTENT = 3;

    private ContentObserver mObserver;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Handler main = new Handler(Looper.getMainLooper());
        mObserver = new ContentObserver(main) {
            @Override
            public void onChange(boolean selfChange, Collection<Uri> uris, int flags) {
                for (Uri uri : uris) Report.line("content: observer onChange " + uri);
            }
        };
        getContentResolver().registerContentObserver(
                Uri.parse("content://com.gesher.devsvc.notes"), true, mObserver);
        JobScheduler js = getSystemService(JobScheduler.class);
        PersistableBundle extras = new PersistableBundle();
        extras.putString("kind", "content");
        int r = js.schedule(new JobInfo.Builder(CONTENT,
                new ComponentName(this, TestJobService.class))
                .addTriggerContentUri(new JobInfo.TriggerContentUri(NotesProvider.ITEMS,
                        JobInfo.TriggerContentUri.FLAG_NOTIFY_FOR_DESCENDANTS))
                .setTriggerContentUpdateDelay(1500)
                .setTriggerContentMaxDelay(6000)
                .setExtras(extras)
                .build());
        Report.line("content: job scheduled (" + r + "), pending reason "
                + js.getPendingJobReason(CONTENT) + Report.pid());
        main.postDelayed(() -> {
            Uri a = getContentResolver().insert(NotesProvider.ITEMS, new ContentValues());
            Report.line("content: inserted " + a);
        }, 2000);
        main.postDelayed(() -> {
            Uri b = getContentResolver().insert(NotesProvider.ITEMS, new ContentValues());
            Report.line("content: inserted " + b);
        }, 2500);
        main.postDelayed(this::finish, 10_000);
    }

    @Override
    protected void onDestroy() {
        getContentResolver().unregisterContentObserver(mObserver);
        super.onDestroy();
    }
}
