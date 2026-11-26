/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.DownloadManager;
import android.provider.Downloads;

import java.io.File;
import java.io.IOException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/**
 * What AOSP's DownloadProvider asks JobScheduler and DownloadJobService to do
 * (Helpers.scheduleJob, DownloadInfo.getRequiredNetworkType/getMinimumLatency): run each
 * ready download once a network it may use is connected, its retry delay has passed and
 * the device is charging or idle when the request requires it (FLAG_REQUIRES_CHARGING,
 * FLAG_REQUIRES_DEVICE_IDLE, read as JobScheduler reads them: {@link DeviceConstraints}),
 * and run it again after a retryable failure. A running download whose charging or idle
 * requirement no longer holds is stopped, as JobScheduler stops its job. It lives in the system server, so a
 * download outlives its app's process; rows persist, so a running download resumes from
 * its last durable checkpoint when Gesher starts again.
 */
final class DownloadQueue {
    /** JobInfo network types used by DownloadInfo.getRequiredNetworkType. */
    static final int NETWORK_TYPE_ANY = 1, NETWORK_TYPE_UNMETERED = 2, NETWORK_TYPE_NOT_ROAMING = 3;
    /** Constants.RETRY_FIRST_DELAY, seconds. */
    static final int RETRY_FIRST_DELAY = 30;
    /** How many downloads transfer at once; the rest stay pending, as queued jobs do. */
    static final int PARALLEL = 3;

    /** The default network now: null when disconnected. */
    static final class NetworkState {
        final boolean metered;
        final boolean roaming;
        NetworkState(boolean metered, boolean roaming) { this.metered = metered; this.roaming = roaming; }
    }

    interface Environment {
        NetworkState network();
        /** Settings.Global.DOWNLOAD_MAX_BYTES_OVER_MOBILE (DownloadManager.getMaxBytesOverMobile). */
        long maxBytesOverMobile();
        /** Settings.Global.DOWNLOAD_RECOMMENDED_MAX_BYTES_OVER_MOBILE. */
        long recommendedMaxBytesOverMobile();
        /** The requesting package's cleartext, trust and connection policy. */
        Transport transport(DownloadRequests.Info info);
        /** Constants.DEFAULT_USER_AGENT. */
        String userAgent();
        long currentTimeMillis();
        long elapsedRealtime();
        /** WindowsHost.powerStatus(), as {@link DeviceConstraints} reads it; null if unknown. */
        int[] powerStatus();
        /** WindowsHost.jobIdleReading(): display-off ms without input; DeviceConstraints.NOT_OFF (-1) unless off. */
        long idleMillis();
        /** Calls {@code listener} when Windows reports a power status or display change. */
        void listen(Runnable listener);
    }

    interface Transport {
        boolean cleartextPermitted(String host);
        javax.net.ssl.SSLSocketFactory socketFactory() throws java.security.GeneralSecurityException;
        java.net.HttpURLConnection open(java.net.URL url) throws IOException;
    }

    private final DownloadRequests mRequests;
    private final Environment mEnvironment;
    private final ExecutorService mWorkers;
    private final ScheduledExecutorService mTimers;
    private final Random mRandom = new Random();
    // Guarded by this.
    private final Map<Long, DownloadTransfer> mRunning = new HashMap<>();
    private final Set<Long> mQueued = new HashSet<>();
    private final Set<Long> mRescheduled = new HashSet<>();
    private final Map<Long, ScheduledFuture<?>> mDelayed = new HashMap<>();
    /** Ready requests waiting for their network or device requirements. */
    private final Set<Long> mWaiting = new HashSet<>();
    /** The running downloads' charging/idle requirements (FLAG_REQUIRES_*). */
    private final Map<Long, Integer> mRunningFlags = new HashMap<>();
    /** One check when a waiting request's inactivity threshold is reached. */
    private ScheduledFuture<?> mIdleCheck;
    private boolean mListening;
    private boolean mStopped;

    DownloadQueue(DownloadRequests requests, Environment environment) {
        mRequests = requests;
        mEnvironment = environment;
        java.util.concurrent.ThreadPoolExecutor workers = new java.util.concurrent.ThreadPoolExecutor(
                PARALLEL, PARALLEL, 30, TimeUnit.SECONDS, new java.util.concurrent.LinkedBlockingQueue<>(),
                task -> {
                    Thread thread = new Thread(task, "gesher-download");
                    thread.setDaemon(true);
                    return thread;
                });
        workers.allowCoreThreadTimeOut(true);
        mWorkers = workers;
        java.util.concurrent.ScheduledThreadPoolExecutor timers =
                new java.util.concurrent.ScheduledThreadPoolExecutor(1, task -> {
                    Thread thread = new Thread(task, "gesher-download-timer");
                    thread.setDaemon(true);
                    return thread;
                });
        timers.setRemoveOnCancelPolicy(true);
        timers.setKeepAliveTime(30, TimeUnit.SECONDS);
        timers.allowCoreThreadTimeOut(true);
        mTimers = timers;
    }

    /** Boot: everything that was ready, including downloads cut off by a restart, runs. */
    void start() {
        boolean listen;
        synchronized (this) {
            listen = !mListening;
            mListening = true;
        }
        if (listen) mEnvironment.listen(this::conditionsChanged);
        for (long id : mRequests.ids()) schedule(id);
    }

    /** Stops scheduling; a running transfer is stopped with a retryable status. */
    void shutdown() {
        synchronized (this) {
            mStopped = true;
            for (DownloadTransfer transfer : mRunning.values()) transfer.requestShutdown();
            for (ScheduledFuture<?> future : mDelayed.values()) future.cancel(false);
            mDelayed.clear();
            if (mIdleCheck != null) mIdleCheck.cancel(false);
            mIdleCheck = null;
        }
        mWorkers.shutdown();
        mTimers.shutdownNow();
    }

    boolean awaitIdle(long timeoutMillis) throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMillis);
        while (System.nanoTime() < deadline) {
            synchronized (this) {
                if (mRunning.isEmpty() && mQueued.isEmpty()) return true;
            }
            Thread.sleep(10);
        }
        return false;
    }

    /** Recheck running and waiting requests when the network or its cost changes. */
    void networkChanged() {
        List<DownloadTransfer> running;
        synchronized (this) {
            if (mStopped) return;
            running = new java.util.ArrayList<>(mRunning.values());
        }
        for (DownloadTransfer transfer : running) transfer.networkChanged();
        reevaluateWaiting();
    }

    /**
     * Windows' power status or session display changed: a running download whose charging
     * or idle requirement no longer holds stops (JobScheduler's onStopJob; its transfer
     * ends with a retryable status and is scheduled again), and waiting requests are
     * evaluated again.
     */
    void conditionsChanged() {
        List<DownloadTransfer> lost = new java.util.ArrayList<>();
        synchronized (this) {
            if (mStopped) return;
            for (Map.Entry<Long, Integer> running : mRunningFlags.entrySet()) {
                if (!deviceConditionsSatisfied(running.getValue())) {
                    DownloadTransfer transfer = mRunning.get(running.getKey());
                    if (transfer != null) lost.add(transfer);
                }
            }
        }
        for (DownloadTransfer transfer : lost) transfer.requestShutdown();
        reevaluateWaiting();
    }

    private void reevaluateWaiting() {
        Set<Long> waiting;
        synchronized (this) {
            waiting = new HashSet<>(mWaiting);
            mWaiting.clear();
            if (mIdleCheck != null) mIdleCheck.cancel(false);
            mIdleCheck = null;
        }
        for (long id : waiting) schedule(id);
    }

    /** A removed or paused download: its transfer stops at its next checkpoint or read. */
    void cancel(long id) {
        synchronized (this) {
            ScheduledFuture<?> delayed = mDelayed.remove(id);
            if (delayed != null) delayed.cancel(false);
            mWaiting.remove(id);
            DownloadTransfer running = mRunning.get(id);
            if (running != null) running.requestShutdown();
        }
    }

    /** Helpers.scheduleJob for one row's current state. */
    void schedule(long id) {
        DownloadRequests.Info info = mRequests.info(id);
        synchronized (this) {
            if (mStopped) return;
            ScheduledFuture<?> delayed = mDelayed.remove(id);
            if (delayed != null) delayed.cancel(false);
            mWaiting.remove(id);
            if (info == null || !info.readyToSchedule()) {
                DownloadTransfer running = mRunning.get(id);
                if (running != null) running.requestShutdown();
                return;
            }
            if (mRunning.containsKey(id) || mQueued.contains(id)) {
                // Evaluated again when the current execution ends.
                mRescheduled.add(id);
                return;
            }
            long latency = minimumLatency(info);
            if (latency > 0) {
                mDelayed.put(id, mTimers.schedule(() -> schedule(id), latency, TimeUnit.MILLISECONDS));
                return;
            }
            if (!networkSatisfied(info, info.totalBytes)) {
                mWaiting.add(id);
                return;
            }
            if (!deviceConditionsSatisfied(info.flags)) {
                mWaiting.add(id);
                armIdleCheck(info.flags);
                return;
            }
            mQueued.add(id);
        }
        try {
            mWorkers.execute(() -> execute(id));
        } catch (java.util.concurrent.RejectedExecutionException stopped) {
            synchronized (this) { mQueued.remove(id); }
        }
    }

    /** DownloadInfo.getRequiredNetworkType. */
    int requiredNetworkType(DownloadRequests.Info info, long totalBytes) {
        if (!info.allowMetered) return NETWORK_TYPE_UNMETERED;
        if (info.allowedNetworkTypes == DownloadManager.Request.NETWORK_WIFI) return NETWORK_TYPE_UNMETERED;
        if (totalBytes > mEnvironment.maxBytesOverMobile()) return NETWORK_TYPE_UNMETERED;
        if (totalBytes > mEnvironment.recommendedMaxBytesOverMobile()
                && info.bypassRecommendedSizeLimit == 0) return NETWORK_TYPE_UNMETERED;
        if (!info.roamingAllowed()) return NETWORK_TYPE_NOT_ROAMING;
        return NETWORK_TYPE_ANY;
    }

    /** Null when the current network carries this request; otherwise why it waits. */
    String networkRefusal(DownloadRequests.Info info, long totalBytes) {
        NetworkState network = mEnvironment.network();
        if (network == null) return "Network is disconnected";
        if (network.roaming && !info.roamingAllowed()) return "Network is roaming";
        if (network.metered && requiredNetworkType(info, totalBytes) == NETWORK_TYPE_UNMETERED) {
            return "Network is metered";
        }
        return null;
    }

    private boolean networkSatisfied(DownloadRequests.Info info, long totalBytes) {
        return networkRefusal(info, totalBytes) == null;
    }

    /**
     * JobInfo.setRequiresCharging / setRequiresDeviceIdle as Helpers.scheduleJob sets them
     * from FLAG_REQUIRES_CHARGING / FLAG_REQUIRES_DEVICE_IDLE, and as BatteryController and
     * IdleController satisfy them (the downloads job is never the top app's).
     */
    boolean deviceConditionsSatisfied(int flags) {
        if ((flags & Downloads.Impl.FLAG_REQUIRES_CHARGING) != 0
                && !DeviceConstraints.stablePower(mEnvironment.powerStatus())) {
            return false;
        }
        if ((flags & Downloads.Impl.FLAG_REQUIRES_DEVICE_IDLE) != 0
                && !DeviceConstraints.idle(mEnvironment.idleMillis())) {
            return false;
        }
        return true;
    }

    /**
     * DeviceIdlenessTracker's idle alarm: with the display already off, idle begins when the
     * inactivity threshold passes, without another display change. Guarded by this.
     */
    private void armIdleCheck(int flags) {
        if ((flags & Downloads.Impl.FLAG_REQUIRES_DEVICE_IDLE) == 0) return;
        long delay = DeviceConstraints.untilIdle(mEnvironment.idleMillis());
        if (delay < 0) return;
        if (mIdleCheck != null) {
            if (mIdleCheck.getDelay(TimeUnit.MILLISECONDS) <= delay) return;
            mIdleCheck.cancel(false);
        }
        mIdleCheck = mTimers.schedule(this::conditionsChanged, delay, TimeUnit.MILLISECONDS);
    }

    /** DownloadInfo.getMinimumLatency. */
    long minimumLatency(DownloadRequests.Info info) {
        if (info.status != Downloads.Impl.STATUS_WAITING_TO_RETRY) return 0;
        long now = mEnvironment.currentTimeMillis();
        long startAfter;
        if (info.numFailed == 0) {
            startAfter = now;
        } else if (info.retryAfter > 0) {
            startAfter = info.lastMod + fuzzDelay(info.retryAfter);
        } else {
            long delay = RETRY_FIRST_DELAY * 1000L * (1L << (info.numFailed - 1));
            startAfter = info.lastMod + fuzzDelay(delay);
        }
        return Math.max(0, startAfter - now);
    }

    /** DownloadInfo.fuzzDelay: {@code delay + sRandom.nextInt((int) (delay / 2))}. */
    private long fuzzDelay(long delay) {
        int bound = (int) (delay / 2);
        return delay + (bound > 0 ? mRandom.nextInt(bound) : 0);
    }

    private void execute(long id) {
        DownloadRequests.Info info = mRequests.info(id);
        DownloadTransfer transfer = null;
        DownloadTransfer.State state = null;
        try {
            synchronized (this) {
                mQueued.remove(id);
                if (mStopped || info == null || !info.readyToSchedule()) return;
                // Skip a download that already finished (DownloadThread.run).
                if (info.status == Downloads.Impl.STATUS_SUCCESS) return;
                // JobScheduler starts a job only while its constraints hold.
                if (!deviceConditionsSatisfied(info.flags)) {
                    mWaiting.add(id);
                    armIdleCheck(info.flags);
                    return;
                }
                state = state(info);
                transfer = new DownloadTransfer(state, network(info), row(info));
                mRunning.put(id, transfer);
                mRunningFlags.put(id, info.flags);
            }
            state.status = Downloads.Impl.STATUS_RUNNING;
            mRequests.writeStatus(id, Downloads.Impl.STATUS_RUNNING);
            transfer.run();
            mRequests.writeState(id, state, false);
        } catch (RuntimeException failure) {
            SystemServer.log("downloads: download " + id + " failed: " + failure);
            if (state != null) {
                state.status = Downloads.Impl.STATUS_UNKNOWN_ERROR;
                state.errorMsg = failure.toString();
                try { mRequests.writeState(id, state, false); }
                catch (RuntimeException ignored) { /* The row's state stays as last written. */ }
            }
        } finally {
            // Completion work happens while the download still counts as running: a
            // retry scheduled from here is deferred to the end of this execution.
            if (state != null) {
                try { finished(id, state.status); }
                catch (RuntimeException e) { SystemServer.log("downloads: finishing " + id + ": " + e); }
            }
            boolean again;
            synchronized (this) {
                if (transfer != null) {
                    mRunning.remove(id);
                    mRunningFlags.remove(id);
                }
                mQueued.remove(id);
                again = mRescheduled.remove(id);
            }
            if (again) schedule(id);
        }
    }

    private void finished(long id, int status) {
        DownloadRequests.Info info = mRequests.info(id);
        if (info == null) return;
        if (Downloads.Impl.isStatusCompleted(status)) {
            // DownloadProvider.update: a completing write sends ACTION_DOWNLOAD_COMPLETE.
            mRequests.sendIntentIfRequested(info);
            return;
        }
        if (status == Downloads.Impl.STATUS_WAITING_TO_RETRY
                || status == Downloads.Impl.STATUS_WAITING_FOR_NETWORK
                || status == Downloads.Impl.STATUS_QUEUED_FOR_WIFI) {
            schedule(id);
        }
    }

    private static DownloadTransfer.State state(DownloadRequests.Info info) {
        DownloadTransfer.State state = new DownloadTransfer.State();
        state.uri = info.uri;
        state.fileName = info.fileName;
        state.mimeType = info.mimeType;
        state.status = info.status;
        state.numFailed = info.numFailed;
        state.retryAfter = info.retryAfter;
        state.totalBytes = info.totalBytes;
        state.currentBytes = info.currentBytes;
        state.etag = info.etag;
        return state;
    }

    private DownloadTransfer.Network network(DownloadRequests.Info info) {
        Transport transport = mEnvironment.transport(info);
        return new DownloadTransfer.Network() {
            @Override public String refusal(long totalBytes) { return networkRefusal(info, totalBytes); }
            @Override public boolean meteredAllowed(long totalBytes) {
                return requiredNetworkType(info, totalBytes) != NETWORK_TYPE_UNMETERED;
            }
            @Override public boolean cleartextPermitted(String host) {
                return transport.cleartextPermitted(host);
            }
            @Override public javax.net.ssl.SSLSocketFactory socketFactory()
                    throws java.security.GeneralSecurityException {
                return transport.socketFactory();
            }
            @Override public java.net.HttpURLConnection open(java.net.URL url) throws IOException {
                return transport.open(url);
            }
        };
    }

    private DownloadTransfer.Row row(DownloadRequests.Info info) {
        return new DownloadTransfer.Row() {
            @Override public List<String[]> headers() { return mRequests.requestHeaders(info); }
            @Override public String userAgent() {
                return info.userAgent != null ? info.userAgent : mEnvironment.userAgent();
            }
            @Override public File reserve(String disposition, String location, String mimeType)
                    throws IOException {
                return mRequests.generateSaveFile(info, info.uri, disposition, location, mimeType);
            }
            @Override public File finalLocation(File running) {
                if (info.destination == Downloads.Impl.DESTINATION_FILE_URI) return running;
                try {
                    File before = mRequests.destinationDirectory(info.destination, true);
                    File after = mRequests.destinationDirectory(info.destination, false);
                    if (!before.equals(after) && before.equals(running.getParentFile())) {
                        return new File(after, running.getName());
                    }
                } catch (IOException | IllegalStateException ignored) {
                    // The file stays where it was written.
                }
                return running;
            }
            @Override public boolean checkpoint(DownloadTransfer.State state) {
                return mRequests.writeState(info.id, state, true);
            }
            @Override public boolean paused() {
                DownloadRequests.Info now = mRequests.info(info.id);
                return now != null && !now.deleted && now.control == Downloads.Impl.CONTROL_PAUSED
                        && now.status != Downloads.Impl.STATUS_CANCELED;
            }
            @Override public long now() { return mEnvironment.elapsedRealtime(); }
        };
    }
}
