/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.microgprobe;

import android.app.job.JobParameters;
import android.app.job.JobService;

public final class ProbeJobService extends JobService {
    @Override public boolean onStartJob(JobParameters params) {
        JobProbe.started(params);
        return false;
    }
    @Override public boolean onStopJob(JobParameters params) { return false; }
}
