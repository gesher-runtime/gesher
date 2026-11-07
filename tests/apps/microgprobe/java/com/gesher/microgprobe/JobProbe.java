/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.microgprobe;

import android.app.Activity;
import android.app.job.IJobScheduler;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.ServiceManager;
import android.util.Log;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Real attached-app Binder authorization and a legitimate own JobService execution. */
final class JobProbe {
    private static final int JOB = 973102;
    private static volatile CountDownLatch completed;
    private static volatile String failure;
    private interface Operation { void run() throws Exception; }
    private static void require(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
    }
    private static void denied(String label, Operation operation) throws Exception {
        try { operation.run(); }
        catch (SecurityException expected) { Log.i("microgprobe", "PASS jobs " + label); return; }
        throw new AssertionError("Unauthorized job operation accepted: " + label);
    }
    static void started(JobParameters params) {
        try {
            require(params.getJobId() == JOB, "Own job ID changed");
            JobWorkItem work = params.dequeueWork();
            require(work != null && "own-work".equals(work.getIntent().getStringExtra("marker")),
                    "Own enqueued job work was lost");
            params.completeWork(work);
        } catch (Throwable problem) { failure = problem.toString(); }
        finally { CountDownLatch latch = completed; if (latch != null) latch.countDown(); }
    }
    static void run(Activity activity) throws Exception {
        IJobScheduler jobs = IJobScheduler.Stub.asInterface(ServiceManager.getService(Context.JOB_SCHEDULER_SERVICE));
        require(jobs != null, "Missing JobScheduler service");
        String own = activity.getPackageName(), victim = "com.gesher.devsvc", namespace = "probe-isolation";
        JobInfo foreign = new JobInfo.Builder(JOB, new ComponentName(victim, victim+".TestJobService"))
                .setMinimumLatency(60000).build();
        require(activity.getPackageManager().getServiceInfo(foreign.getService(), 0) != null,
                "Foreign job fixture must actually be installed");
        denied("foreign schedule", () -> jobs.schedule(namespace, foreign));
        denied("foreign enqueue", () -> jobs.enqueue(namespace, foreign, new JobWorkItem(new Intent("probe.foreign"))));
        denied("scheduleAsPackage", () -> jobs.scheduleAsPackage(namespace, foreign, victim, 0, "probe"));
        denied("scheduleAsPackage user forgery", () -> jobs.scheduleAsPackage(namespace, foreign, own, 10, "probe"));
        denied("global started jobs", () -> jobs.getStartedJobs());
        denied("global job snapshots", () -> jobs.getAllJobSnapshots());
        denied("global observer register", () -> jobs.registerUserVisibleJobObserver(null));
        denied("global observer unregister", () -> jobs.unregisterUserVisibleJobObserver(null));
        denied("global app-stop mutation", () -> jobs.notePendingUserRequestedAppStop(victim, 0, "probe"));
        denied("claimed job-permission package", () -> jobs.canRunUserInitiatedJobs(victim));
        denied("claimed job-permission user", () -> jobs.hasRunUserInitiatedJobsPermission(own, 10));

        JobInfo ownPending = new JobInfo.Builder(JOB, new ComponentName(activity, ProbeJobService.class))
                .setMinimumLatency(60000).build();
        require(jobs.schedule(namespace, ownPending) == JobScheduler.RESULT_SUCCESS, "Own job rejected");
        require(jobs.getPendingJob(namespace, JOB) != null, "Own pending job not visible");
        require(jobs.getPendingJob(victim, JOB) == null, "A namespace was treated as another package identity");
        require(jobs.getAllPendingJobsInNamespace(victim).getList().isEmpty(), "Foreign namespace leaked jobs");
        jobs.cancel(victim, JOB);
        jobs.cancelAllInNamespace(victim);
        require(jobs.getPendingJob(namespace, JOB) != null, "Namespace forgery cancelled an unrelated own job");
        for (android.content.pm.ParceledListSlice<JobInfo> slice : jobs.getAllPendingJobs().values()) {
            for (JobInfo job : slice.getList()) require(own.equals(job.getService().getPackageName()),
                    "Package-scoped all-jobs query leaked another owner");
        }
        jobs.cancelAll(); // Scoped to this authenticated package, across its namespaces.
        require(jobs.getPendingJob(namespace, JOB) == null, "Own cancellation failed");
        Log.i("microgprobe", "PASS jobs authenticated queries/cancel and namespace isolation");

        completed = new CountDownLatch(1); failure = null;
        try {
            JobInfo ready = new JobInfo.Builder(JOB, ownPending.getService()).setMinimumLatency(100).build();
            require(jobs.enqueue(namespace, ready, new JobWorkItem(new Intent("probe.own").putExtra("marker", "own-work")))
                            == JobScheduler.RESULT_SUCCESS, "Own enqueued job rejected");
            require(completed.await(30, TimeUnit.SECONDS), "Own JobService did not run");
            require(failure == null, "Own JobService callback failed: "+failure);
            Log.i("microgprobe", "PASS jobs own JobService ran and completed authenticated work");
        } finally { jobs.cancel(namespace, JOB); completed = null; }
    }
}
