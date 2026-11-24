/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * {@link HelloKeyBackend} over Windows' KeyCredentialManager (ADR 0095;
 * compat/devices/hello_key_jni.cc and hello_key_win.cc). Every status, public key and
 * signature is Windows' own; a prompt is owned by the requesting app's foreground window.
 *
 * <p>No JNI callbacks: a signature has a daemon thread that blocks in {@code nativeAwait}
 * and then reports, once, through the completion.
 */
final class WindowsHelloKey implements HelloKeyBackend {
    private final Object mLock = new Object();
    private int mNextId;

    private WindowsHelloKey() {
    }

    /** The backend, or null when the natives are not in this runtime (host tests, old builds). */
    static WindowsHelloKey create() {
        try {
            nativeSupported(new int[1]);
            return new WindowsHelloKey();
        } catch (UnsatisfiedLinkError unavailable) {
            return null;
        }
    }

    @Override
    public boolean isSupported() {
        return nativeSupported(new int[1]) == 1;  // a failed query is not "supported"
    }

    @Override
    public Result create(String name, int pid) {
        return new Request(nativeStartCreate(pid, name)).await();
    }

    @Override
    public Result open(String name) {
        byte[][] publicKey = new byte[1][];
        int status = status(nativeOpen(name, publicKey, new int[1]));
        if (status == SUCCESS && publicKey[0] == null) status = FAILED;
        return new Result(status, status == SUCCESS ? publicKey[0] : null);
    }

    @Override
    public Operation sign(String name, int pid, byte[] data, Completion completion) {
        int id;
        synchronized (mLock) {
            id = ++mNextId;
        }
        Request request = new Request(nativeStartSign(pid, name, data));
        // On its own thread also when nothing started: never before sign returns.
        Thread t = new Thread(() -> completion.onResult(request.await()), "keystore-hello-" + id);
        t.setDaemon(true);
        t.start();
        return request;
    }

    @Override
    public int delete(String name) {
        return status(nativeDelete(name, new int[1]));
    }

    /** Windows' status, or FAILED for a value this code does not know. */
    private static int status(int value) {
        return value >= NOT_FOREGROUND && value <= SECURITY_DEVICE_LOCKED ? value : FAILED;
    }

    /** One native request: awaited once, released once, cancel safe until then. */
    private static final class Request implements Operation {
        private final long mHandle;
        private boolean mReleased;

        Request(long handle) {
            mHandle = handle;
        }

        Result await() {
            if (mHandle == 0) return new Result(FAILED, null);
            byte[][] bytes = new byte[1][];
            int status = status(nativeAwait(mHandle, bytes));
            synchronized (this) {
                mReleased = true;
                nativeRelease(mHandle);
            }
            if (status == SUCCESS && bytes[0] == null) status = FAILED;
            return new Result(status, status == SUCCESS ? bytes[0] : null);
        }

        @Override
        public void cancel() {
            synchronized (this) {
                if (!mReleased && mHandle != 0) nativeCancel(mHandle);
            }
        }
    }

    private static native int nativeSupported(int[] hr);

    private static native int nativeOpen(String name, byte[][] publicKey, int[] hr);

    private static native int nativeDelete(String name, int[] hr);

    private static native long nativeStartCreate(int pid, String name);

    private static native long nativeStartSign(int pid, String name, byte[] data);

    private static native int nativeAwait(long handle, byte[][] bytes);

    private static native void nativeCancel(long handle);

    private static native void nativeRelease(long handle);
}
