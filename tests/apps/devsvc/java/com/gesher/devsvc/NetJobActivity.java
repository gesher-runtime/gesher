/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PersistableBundle;

/**
 * Network constraints over ConnectivityManager's view of Windows' networks (hwsvc2, ADR
 * 0051): a job needing any network and one needing an unmetered network run (the runner's
 * Ethernet is both), with their network in JobParameters; one needing a cellular network
 * waits (reason CONNECTIVITY) and is cancelled. Finishes after 15 s.
 */
public class NetJobActivity extends Activity {
    static final int ANY = 4;
    static final int UNMETERED = 5;
    static final int CELLULAR = 6;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        JobScheduler js = getSystemService(JobScheduler.class);
        ComponentName service = new ComponentName(this, TestJobService.class);
        int[][] jobs = {{ANY, JobInfo.NETWORK_TYPE_ANY}, {UNMETERED, JobInfo.NETWORK_TYPE_UNMETERED},
                {CELLULAR, JobInfo.NETWORK_TYPE_CELLULAR}};
        String[] kinds = {"network any", "network unmetered", "network cellular"};
        for (int i = 0; i < jobs.length; i++) {
            PersistableBundle extras = new PersistableBundle();
            extras.putString("kind", kinds[i]);
            int r = js.schedule(new JobInfo.Builder(jobs[i][0], service)
                    .setRequiredNetworkType(jobs[i][1]).setExtras(extras).build());
            Report.line("netjobs: scheduled " + jobs[i][0] + " (" + kinds[i] + "): " + r);
        }
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            Report.line("netjobs: cellular job pending reason " + js.getPendingJobReason(CELLULAR)
                    + " (connectivity is " + JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY
                    + ")");
            js.cancel(CELLULAR);
            Report.line("netjobs: done");
            finish();
        }, 15_000);
    }
}
