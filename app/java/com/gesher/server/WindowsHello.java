/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * {@link WindowsHelloBackend} over Windows' UserConsentVerifier (ADR 0091, phase 2;
 * compat/devices/hello_jni.cc and hello_win.cc, docs/research/windows-hello-native-spec.md).
 * Every result is Windows' own: a prompt owned by the requesting app's foreground window,
 * Verified only when Windows says so.
 *
 * <p>No JNI callbacks: each verification has a daemon thread that blocks in
 * {@code nativeAwait} and then reports, once, through the completion. A second daemon
 * thread watches the owner while Windows' prompt shows ({@link OwnerWatch}): when the app
 * leaves the foreground or its window is gone, the verification is canceled, and Windows'
 * Canceled is what the completion reports.
 */
final class WindowsHello implements WindowsHelloBackend {
    /** nativeStart's out[1]: why no verification started. */
    private static final int REASON_NOT_FOREGROUND = 1;
    /** Availability answers are cached this long (the query waits up to 2 s natively). */
    private static final long AVAILABILITY_CACHE_MS = 5000;
    /** How often the prompt's owner is looked at. */
    private static final long OWNER_WATCH_MS = 250;
    /** After this long without seeing the prompt as an owned window, say so once. */
    private static final long PROMPT_SEEN_MS = 3000;

    private final Object mLock = new Object();
    private long mAvailabilityAt;
    private int mAvailability = FAILED;
    private int mNextId;

    private WindowsHello() {
    }

    /** The backend, or null when the natives are not in this runtime (host tests, old builds). */
    static WindowsHello create() {
        try {
            nativeAvailability(new int[1]);
            return new WindowsHello();
        } catch (UnsatisfiedLinkError unavailable) {
            return null;
        }
    }

    /** Windows' build number (RtlGetVersion), 0 when it cannot be read. */
    static int windowsBuild() {
        try {
            return nativeWindowsBuild();
        } catch (UnsatisfiedLinkError unavailable) {
            return 0;
        }
    }

    @Override
    public int availability() {
        long now = System.nanoTime() / 1_000_000;
        synchronized (mLock) {
            if (mAvailabilityAt != 0 && now - mAvailabilityAt < AVAILABILITY_CACHE_MS) {
                return mAvailability;
            }
        }
        int[] hr = new int[1];
        int value = nativeAvailability(hr);
        if (value < AVAILABLE || value > DEVICE_BUSY) value = FAILED;
        synchronized (mLock) {
            mAvailability = value;
            mAvailabilityAt = now == 0 ? 1 : now;
        }
        return value;
    }

    @Override
    public Verification verify(Request request, Completion completion) {
        int id;
        synchronized (mLock) {
            id = ++mNextId;
        }
        int[] out = new int[2];
        // The window Windows' prompt will be owned by, for the watch below. nativeStart
        // makes the same choice itself; 0 here is its "not in the foreground".
        long owner = nativeOwnerWindow(request.pid);
        long handle = owner == 0 ? 0 : nativeStart(request.pid, request.message, out);
        if (owner == 0) out[1] = REASON_NOT_FOREGROUND;
        if (handle == 0) {
            // The interface forbids completing before verify returns.
            int result = out[1] == REASON_NOT_FOREGROUND ? CANCELED : FAILED;
            Thread t = new Thread(() -> completion.onResult(result), "auth-hello-" + id);
            t.setDaemon(true);
            t.start();
            return () -> { };
        }
        Session session = new Session(handle);
        java.util.concurrent.atomic.AtomicBoolean done =
                new java.util.concurrent.atomic.AtomicBoolean();
        Thread t = new Thread(() -> {
            int result = session.await();
            done.set(true);
            completion.onResult(result);
        }, "auth-hello-" + id);
        t.setDaemon(true);
        t.start();
        Thread watch = new Thread(() -> watchOwner(owner, request.pid, session, done),
                "auth-hello-owner-" + id);
        watch.setDaemon(true);
        watch.start();
        return session;
    }

    /**
     * Cancels {@code verification} when its owner leaves the foreground or is gone
     * (SystemUI's cancelIfOwnerIsNotInForeground); ends when the verification does.
     */
    private static void watchOwner(long owner, int pid, Verification verification,
            java.util.concurrent.atomic.AtomicBoolean done) {
        OwnerWatch watch = new OwnerWatch();
        long start = System.nanoTime();
        boolean reported = false;
        while (!done.get()) {
            int relation = nativeOwnerRelation(owner, pid);
            if (watch.sample(relation)) {
                SystemServer.log("auth: the owner of Windows Hello's prompt (pid " + pid + ") "
                        + (relation == OwnerWatch.GONE ? "is gone" : "left the foreground")
                        + ": canceling the verification");
                verification.cancel();
                return;
            }
            if (!reported && !watch.promptSeen() && relation == OwnerWatch.OTHER
                    && (System.nanoTime() - start) / 1_000_000 >= PROMPT_SEEN_MS) {
                reported = true;
                SystemServer.log("auth: Windows Hello's prompt was not seen as a window owned by"
                        + " pid " + pid + "; not canceling on a foreground change");
            }
            try {
                Thread.sleep(OWNER_WATCH_MS);
            } catch (InterruptedException interrupted) {
                return;
            }
        }
    }

    /** One native verification: awaited once, released once, cancel safe until then. */
    private static final class Session implements Verification {
        private final long mHandle;
        private boolean mReleased;

        Session(long handle) {
            mHandle = handle;
        }

        int await() {
            int result = nativeAwait(mHandle);
            synchronized (this) {
                mReleased = true;
                nativeRelease(mHandle);
            }
            return result < VERIFIED || result > CANCELED ? FAILED : result;
        }

        @Override
        public void cancel() {
            synchronized (this) {
                if (!mReleased) nativeCancel(mHandle);
            }
        }
    }

    private static native int nativeAvailability(int[] hr);

    private static native int nativeWindowsBuild();

    /** The foreground window of {@code pid} (its root owner) as a handle value, or 0. */
    private static native long nativeOwnerWindow(int pid);

    /** Where Windows' foreground window is relative to {@code owner}: an OwnerWatch value. */
    private static native int nativeOwnerRelation(long owner, int pid);

    private static native long nativeStart(int pid, String message, int[] out);

    private static native int nativeAwait(long handle);

    private static native void nativeCancel(long handle);

    private static native void nativeRelease(long handle);
}
