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
        int id = nextId();
        long owner = WindowsHello.nativeOwnerWindow(pid);
        Request request = startOwned(owner, () -> nativeStartSign(pid, name, data));
        // On its own thread also when nothing started: never before sign returns.
        Thread t = new Thread(() -> completion.onResult(awaitOwned(request, owner, pid, id)),
                "keystore-hello-" + id);
        t.setDaemon(true);
        t.start();
        return request;
    }

    /** Never start a prompt without the owner that its lifecycle watch will follow. */
    static Request startOwned(long owner, java.util.function.LongSupplier nativeStart) {
        if (owner == 0) {
            return new Request(() -> new Result(NOT_FOREGROUND, null), () -> { }, () -> { });
        }
        return new Request(nativeStart.getAsLong());
    }

    private int nextId() {
        synchronized (mLock) {
            return ++mNextId;
        }
    }

    /** Key-backed authentication has the same foreground lifetime as the plain verifier. */
    private static Result awaitOwned(Request request, long owner, int pid, int id) {
        java.util.concurrent.atomic.AtomicBoolean done =
                new java.util.concurrent.atomic.AtomicBoolean();
        if (owner != 0) {
            Thread watch = new Thread(
                    () -> WindowsHello.watchOwner(owner, pid, request::cancel, done),
                    "keystore-hello-owner-" + id);
            watch.setDaemon(true);
            watch.start();
        }
        try {
            return request.await();
        } finally {
            done.set(true);
        }
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
    static final class Request implements Operation {
        private final java.util.function.Supplier<Result> mAwait;
        private final Runnable mCancel;
        private final Runnable mRelease;
        private boolean mReleased;
        private boolean mCanceled;

        Request(long handle) {
            this(() -> awaitNative(handle),
                    () -> { if (handle != 0) nativeCancel(handle); },
                    () -> { if (handle != 0) nativeRelease(handle); });
        }

        /** Native request boundary, injectable without a Windows prompt in host tests. */
        Request(java.util.function.Supplier<Result> await, Runnable cancel, Runnable release) {
            mAwait = await;
            mCancel = cancel;
            mRelease = release;
        }

        Result await() {
            Result result;
            try {
                result = mAwait.get();
            } finally {
                synchronized (this) {
                    mReleased = true;
                    mRelease.run();
                }
            }
            synchronized (this) {
                if (mCanceled) {
                    if (result.data != null) java.util.Arrays.fill(result.data, (byte) 0);
                    return new Result(USER_CANCELED, null);
                }
            }
            return result;
        }

        @Override
        public void cancel() {
            synchronized (this) {
                if (mReleased || mCanceled) return;
                mCanceled = true;
                mCancel.run();
            }
        }
    }

    private static Result awaitNative(long handle) {
        if (handle == 0) return new Result(FAILED, null);
        byte[][] bytes = new byte[1][];
        int status = status(nativeAwait(handle, bytes));
        if (status == SUCCESS && bytes[0] == null) status = FAILED;
        return new Result(status, status == SUCCESS ? bytes[0] : null);
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
