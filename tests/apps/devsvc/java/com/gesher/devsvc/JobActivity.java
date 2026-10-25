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
import android.os.PersistableBundle;

/**
 * Jobs: a one-off job with a minimum latency and a deadline, and a periodic job (its first
 * period runs at once: its flex is the whole interval), both run by TestJobService after this
 * activity finished and its process ended.
 */
public class JobActivity extends Activity {
    static final int ONE_OFF = 1;
    static final int PERIODIC = 2;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        JobScheduler js = getSystemService(JobScheduler.class);
        ComponentName service = new ComponentName(this, TestJobService.class);
        PersistableBundle extras = new PersistableBundle();
        extras.putString("kind", "one-off");
        int a = js.schedule(new JobInfo.Builder(ONE_OFF, service).setMinimumLatency(8000)
                .setOverrideDeadline(30000).setRequiresCharging(true).setExtras(extras).build());
        extras.putString("kind", "periodic");
        long interval = JobInfo.getMinPeriodMillis();
        int b = js.schedule(new JobInfo.Builder(PERIODIC, service)
                .setPeriodic(interval, interval).setExtras(extras).build());
        Report.line("jobs: scheduled (" + a + ", " + b + "), pending "
                + js.getAllPendingJobs().size() + Report.pid());
        finish();
    }
}
