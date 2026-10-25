/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;

/** Runs the jobs on a thread of its own and reports them finished; cancels the periodic one after two runs. */
public class TestJobService extends JobService {
    @Override
    public void onCreate() {
        super.onCreate();
        Report.line("jobs: TestJobService onCreate" + Report.pid());
    }

    @Override
    public boolean onStartJob(JobParameters params) {
        String kind = params.getExtras().getString("kind");
        int run = Report.count(this, "job" + params.getJobId());
        Report.line("jobs: onStartJob " + params.getJobId() + " (" + kind + ") run " + run
                + Report.pid());
        if (params.getTriggeredContentAuthorities() != null) {
            Report.line("jobs: triggered by " + java.util.Arrays.toString(
                    params.getTriggeredContentUris()) + " authorities "
                    + java.util.Arrays.toString(params.getTriggeredContentAuthorities()));
        }
        new Thread(() -> {
            try {
                Thread.sleep(500);
            } catch (InterruptedException e) {
                return;
            }
            if (params.getJobId() == JobActivity.PERIODIC && run >= 2) {
                getSystemService(JobScheduler.class).cancel(JobActivity.PERIODIC);
                Report.line("jobs: periodic job cancelled after " + run + " runs");
            }
            Report.line("jobs: job " + params.getJobId() + " finished");
            jobFinished(params, false);
        }).start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        Report.line("jobs: onStopJob " + params.getJobId());
        return false;
    }

    @Override
    public void onDestroy() {
        Report.line("jobs: TestJobService onDestroy");
        super.onDestroy();
    }
}
