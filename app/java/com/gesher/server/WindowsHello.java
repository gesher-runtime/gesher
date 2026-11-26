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
 * {@code nativeAwait} and then reports, once, through the completion.
 */
final class WindowsHello implements WindowsHelloBackend {
    /** nativeStart's out[1]: why no verification started. */
    private static final int REASON_NOT_FOREGROUND = 1;
    /** Availability answers are cached this long (the query waits up to 2 s natively). */
    private static final long AVAILABILITY_CACHE_MS = 5000;

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
        long handle = nativeStart(request.pid, request.message, out);
        if (handle == 0) {
            // The interface forbids completing before verify returns.
            int result = out[1] == REASON_NOT_FOREGROUND ? CANCELED : FAILED;
            Thread t = new Thread(() -> completion.onResult(result), "auth-hello-" + id);
            t.setDaemon(true);
            t.start();
            return () -> { };
        }
        Session session = new Session(handle);
        Thread t = new Thread(() -> completion.onResult(session.await()), "auth-hello-" + id);
        t.setDaemon(true);
        t.start();
        return session;
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

    private static native long nativeStart(int pid, String message, int[] out);

    private static native int nativeAwait(long handle);

    private static native void nativeCancel(long handle);

    private static native void nativeRelease(long handle);
}
