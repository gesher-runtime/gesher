/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.SystemClock;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/**
 * Coalesces immutable policy snapshots like Android 16 AccessPersistence. Publication belongs
 * to the policy; a failed disk write never rolls it back or escapes into an ordinary mutation.
 */
final class PolicyStateWriter<T> {
    interface Serializer<T> { void write(T state) throws Exception; }
    private static final long WRITE_DELAY_MILLIS = 1000, MAX_WRITE_DELAY_MILLIS = 2000;
    private static final ScheduledThreadPoolExecutor IO = new ScheduledThreadPoolExecutor(1, task -> {
        Thread thread = new Thread(task, "gesher-policy-io");
        thread.setDaemon(true);
        return thread;
    });
    static {
        IO.setRemoveOnCancelPolicy(true);
        IO.setKeepAliveTime(30, TimeUnit.SECONDS);
        IO.allowCoreThreadTimeOut(true);
    }

    private final Object mScheduleLock = new Object();
    private final Object mWriteLock = new Object();
    private final String mName;
    private final Serializer<T> mSerializer;
    private T mLatest;
    private long mVersion, mWrittenVersion;
    private long mFirstMutation = -1;
    private ScheduledFuture<?> mTask;

    PolicyStateWriter(String name, Serializer<T> serializer) {
        mName = name;
        mSerializer = serializer;
    }

    /** The caller must not modify this snapshot after handing it to the writer. */
    void schedule(T snapshot) {
        synchronized (mScheduleLock) {
            mLatest = snapshot;
            mVersion++;
            long now = SystemClock.uptimeMillis();
            if (mFirstMutation < 0) mFirstMutation = now;
            if (mTask != null) mTask.cancel(false);
            long delay = Math.max(0, Math.min(WRITE_DELAY_MILLIS,
                    MAX_WRITE_DELAY_MILLIS - (now - mFirstMutation)));
            mTask = IO.schedule(() -> { flush(); }, delay, TimeUnit.MILLISECONDS);
        }
    }

    /**
     * Explicit durability boundary. Never call while holding a package or operation monitor.
     * Failure remains dirty for a later mutation or explicit retry; there is no retry spin.
     */
    boolean flush() {
        synchronized (mWriteLock) {
            T snapshot;
            long version;
            synchronized (mScheduleLock) {
                if (mTask != null) mTask.cancel(false);
                mTask = null;
                mFirstMutation = -1;
                if (mLatest == null || mWrittenVersion == mVersion) return true;
                snapshot = mLatest;
                version = mVersion;
            }
            try {
                mSerializer.write(snapshot);
            } catch (Exception failure) {
                SystemServer.log(mName + ": cannot persist policy: " + failure);
                return false;
            }
            synchronized (mScheduleLock) { mWrittenVersion = version; }
            return true;
        }
    }
}
