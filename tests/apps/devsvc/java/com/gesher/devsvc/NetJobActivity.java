/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PersistableBundle;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeSet;

/**
 * Network constraints over ConnectivityManager's view of Windows' networks (hwsvc2, ADR
 * 0051). Record the real networks satisfying each public JobInfo request before and
 * after the observation window. Eligible jobs must run with one of those networks;
 * ineligible jobs must wait for connectivity. Ethernet alone does not prove an
 * unmetered connection, especially when Windows cannot report its cost. Finishes
 * after 15 s and cancels every remaining fixture job.
 */
public class NetJobActivity extends Activity {
    static final int ANY = 4;
    static final int UNMETERED = 5;
    static final int CELLULAR = 6;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        JobScheduler js = getSystemService(JobScheduler.class);
        ConnectivityManager cm = getSystemService(ConnectivityManager.class);
        ComponentName service = new ComponentName(this, TestJobService.class);
        int[][] jobs = {{ANY, JobInfo.NETWORK_TYPE_ANY}, {UNMETERED, JobInfo.NETWORK_TYPE_UNMETERED},
                {CELLULAR, JobInfo.NETWORK_TYPE_CELLULAR}};
        String[] kinds = {"network any", "network unmetered", "network cellular"};
        JobInfo[] requests = new JobInfo[jobs.length];
        for (int i = 0; i < jobs.length; i++) {
            PersistableBundle extras = new PersistableBundle();
            extras.putString("kind", kinds[i]);
            requests[i] = new JobInfo.Builder(jobs[i][0], service)
                    .setRequiredNetworkType(jobs[i][1]).setExtras(extras).build();
        }
        reportMatches("before", cm, requests);
        for (int i = 0; i < jobs.length; i++) {
            int r = js.schedule(requests[i]);
            Report.line("netjobs: scheduled " + jobs[i][0] + " (" + kinds[i] + "): " + r);
        }
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            reportMatches("after", cm, requests);
            for (int[] job : jobs) {
                Report.line("netjobs: job " + job[0] + " pending reason "
                        + js.getPendingJobReason(job[0]) + " (connectivity is "
                        + JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY + ")");
                js.cancel(job[0]);
                Report.line("netjobs: cancelled " + job[0] + ", pending "
                        + (js.getPendingJob(job[0]) != null));
            }
            Report.line("netjobs: done");
            finish();
        }, 15_000);
    }

    private static void reportMatches(String phase, ConnectivityManager cm, JobInfo[] requests) {
        Map<Network, NetworkCapabilities> networks = new LinkedHashMap<>();
        for (Network network : cm.getAllNetworks()) {
            NetworkCapabilities capabilities = cm.getNetworkCapabilities(network);
            networks.put(network, capabilities);
            Report.line("netjobs: " + phase + " network " + network + " capabilities " + capabilities);
        }
        for (JobInfo request : requests) {
            TreeSet<String> matches = new TreeSet<>();
            for (Map.Entry<Network, NetworkCapabilities> network : networks.entrySet()) {
                if (request.getRequiredNetwork().canBeSatisfiedBy(network.getValue())) {
                    matches.add(network.getKey().toString());
                }
            }
            Report.line("netjobs: " + phase + " matches " + request.getId()
                    + " [" + String.join(",", matches) + "]");
        }
    }
}
