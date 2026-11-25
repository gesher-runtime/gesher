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
    private final java.util.function.IntToLongFunction mOwner;
    private final java.util.function.BiFunction<String, Integer, Request> mCreate;
    private final OwnerWatcher mOwnerWatcher;
    private final WatchStarter mWatchStarter;

    private WindowsHelloKey() {
        this(WindowsHello::nativeOwnerWindow,
                (name, pid) -> new Request(nativeStartCreate(pid, name)),
                (owner, pid, cancel, done) ->
                        WindowsHello.watchOwner(owner, pid, cancel::run, done));
    }

    /** Native ownership boundary, injectable without a Windows prompt in host tests. */
    WindowsHelloKey(java.util.function.IntToLongFunction owner,
            java.util.function.BiFunction<String, Integer, Request> create,
            OwnerWatcher ownerWatcher) {
        this(owner, create, ownerWatcher, (work, name) -> {
            Thread watch = new Thread(work, name);
            watch.setDaemon(true);
            watch.start();
        });
    }

    WindowsHelloKey(java.util.function.IntToLongFunction owner,
            java.util.function.BiFunction<String, Integer, Request> create,
            OwnerWatcher ownerWatcher, WatchStarter watchStarter) {
        mOwner = owner;
        mCreate = create;
        mOwnerWatcher = ownerWatcher;
        mWatchStarter = watchStarter;
    }

    interface OwnerWatcher {
        void watch(long owner, int pid, Runnable cancel,
                java.util.concurrent.atomic.AtomicBoolean done);
    }

    interface WatchStarter {
        void start(Runnable work, String name);
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
        long owner = mOwner.applyAsLong(pid);
        Request request = owner == 0 ? startOwned(0, () -> 0) : mCreate.apply(name, pid);
        return awaitOwned(request, owner, pid, nextId());
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
        long owner = mOwner.applyAsLong(pid);
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
    private Result awaitOwned(Request request, long owner, int pid, int id) {
        java.util.concurrent.atomic.AtomicBoolean done =
                new java.util.concurrent.atomic.AtomicBoolean();
        try {
            if (owner != 0) {
                try {
                    mWatchStarter.start(() -> {
                        try {
                            mOwnerWatcher.watch(owner, pid, request::cancel, done);
                        } catch (RuntimeException | Error failure) {
                            cancelAfterWatchFailure(request, failure);
                            // An uncaught watcher exception would terminate Android's
                            // system server. Reject its request and report the failed watch.
                            System.err.println("gesher hello key: owner watch failed: " + failure);
                        }
                    }, "keystore-hello-owner-" + id);
                } catch (RuntimeException | Error failure) {
                    // Native creation already owns a request. Even failure to allocate or
                    // start its watcher must deliver one rejected native disposition.
                    cancelAfterWatchFailure(request, failure);
                    try {
                        request.await();
                    } catch (RuntimeException | Error cleanupFailure) {
                        if (cleanupFailure != failure) failure.addSuppressed(cleanupFailure);
                    }
                    throw failure;
                }
            }
            return request.await();
        } finally {
            done.set(true);
        }
    }

    private static void cancelAfterWatchFailure(Request request, Throwable failure) {
        try {
            request.cancel();
        } catch (RuntimeException | Error cancelFailure) {
            if (cancelFailure != failure) failure.addSuppressed(cancelFailure);
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
        private final java.util.function.Consumer<Boolean> mRelease;
        private boolean mReleased;
        private boolean mCanceled;

        Request(long handle) {
            this(() -> awaitNative(handle),
                    () -> { if (handle != 0) nativeCancel(handle); },
                    accepted -> { if (handle != 0) nativeRelease(handle, accepted); });
        }

        /** Native request boundary, injectable without a Windows prompt in host tests. */
        Request(java.util.function.Supplier<Result> await, Runnable cancel, Runnable release) {
            this(await, cancel, accepted -> release.run());
        }

        Request(java.util.function.Supplier<Result> await, Runnable cancel,
                java.util.function.Consumer<Boolean> release) {
            mAwait = await;
            mCancel = cancel;
            mRelease = release;
        }

        Result await() {
            try {
                Result result = mAwait.get();
                synchronized (this) {
                    if (mCanceled) {
                        if (result.data != null) java.util.Arrays.fill(result.data, (byte) 0);
                        result = new Result(USER_CANCELED, null);
                    }
                    // Delivery and cancellation have one disposition. Native creation
                    // retains cleanup ownership until this decision, including a failed
                    // JNI byte-array handoff or a late successful canceled create.
                    mReleased = true;
                    mRelease.accept(result.status == SUCCESS);
                }
                return result;
            } finally {
                synchronized (this) {
                    if (!mReleased) {
                        mReleased = true;
                        mRelease.accept(false);
                    }
                }
            }
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

    private static native void nativeRelease(long handle, boolean accepted);
}
