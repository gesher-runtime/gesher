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
    private final SignStarter mSign;

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
        this(owner, create, ownerWatcher, watchStarter,
                (name, pid, data) -> new Request(nativeStartSign(pid, name, data)));
    }

    WindowsHelloKey(java.util.function.IntToLongFunction owner,
            java.util.function.BiFunction<String, Integer, Request> create,
            OwnerWatcher ownerWatcher, WatchStarter watchStarter, SignStarter sign) {
        mOwner = owner;
        mCreate = create;
        mOwnerWatcher = ownerWatcher;
        mWatchStarter = watchStarter;
        mSign = sign;
    }

    interface OwnerWatcher {
        void watch(long owner, int pid, Runnable cancel,
                java.util.concurrent.atomic.AtomicBoolean done);
    }

    interface WatchStarter {
        /** Starts work once, or throws before it starts. */
        void start(Runnable work, String name);
    }

    interface SignStarter {
        Request start(String name, int pid, byte[] data);
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
    public Creation create(String name, int pid) {
        long owner = mOwner.applyAsLong(pid);
        Request request = owner == 0 ? startOwned(0, () -> 0) : mCreate.apply(name, pid);
        try {
            Result result = awaitOwned(request, owner, pid, nextId(), true);
            return new Creation(result, request::disposeCreation);
        } catch (RuntimeException | Error failure) {
            // Also covers allocation of the Java transaction after native completion.
            // Native cleanup must retain the name even when no Creation is returned.
            request.disposeCreation(false);
            throw failure;
        }
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
        Request request = owner == 0 ? startOwned(0, () -> 0) : mSign.start(name, pid, data);
        // Completion runs on its own thread also when no native prompt started.
        try {
            mWatchStarter.start(() -> {
                Result result;
                try {
                    result = awaitOwned(request, owner, pid, id);
                } catch (RuntimeException | Error failure) {
                    rejectAfterWaiterFailure(request, failure);
                    // The caller is already waiting for its result. An owner-watch
                    // startup or native handoff failure must not escape this server
                    // thread or leave the enrollment waiting for a callback forever.
                    System.err.println("gesher hello key: signature wait failed: " + failure);
                    result = new Result(FAILED, null);
                }
                completion.onResult(result);
            }, "keystore-hello-" + id);
        } catch (RuntimeException | Error failure) {
            // A native signature may already be running when allocating/starting
            // its waiter fails. Reject that request before propagating the failure.
            rejectAfterWaiterFailure(request, failure);
            throw failure;
        }
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
        return awaitOwned(request, owner, pid, id, false);
    }

    private Result awaitOwned(Request request, long owner, int pid, int id,
            boolean retainCreation) {
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
            return request.await(retainCreation);
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

    /** Called only by the waiter or when its starter failed before starting it. */
    private static void rejectAfterWaiterFailure(Request request, Throwable failure) {
        synchronized (request) {
            // awaitOwned may already have rejected and released the native handle.
            // It must never be awaited again after either successful or failed handoff.
            if (request.mReleased) return;
        }
        cancelAfterWatchFailure(request, failure);
        try {
            request.await();
        } catch (RuntimeException | Error cleanupFailure) {
            if (cleanupFailure != failure) failure.addSuppressed(cleanupFailure);
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

    /** One native request: awaited once, released once, canceled only before delivery. */
    static final class Request implements Operation {
        private final java.util.function.Supplier<Result> mAwait;
        private final Runnable mCancel;
        private final java.util.function.Consumer<Boolean> mRelease;
        private boolean mReleased;
        private boolean mCanceled;
        private boolean mDelivered;

        Request(long handle) {
            try {
                mAwait = () -> awaitNative(handle);
                mCancel = () -> { if (handle != 0) nativeCancel(handle); };
                mRelease = accepted -> { if (handle != 0) nativeRelease(handle, accepted); };
            } catch (RuntimeException | Error failure) {
                if (handle != 0) nativeRelease(handle, false);
                throw failure;
            }
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
            return await(false);
        }

        Result await(boolean retainCreation) {
            try {
                Result result = mAwait.get();
                synchronized (this) {
                    if (mCanceled) {
                        if (result.data != null) java.util.Arrays.fill(result.data, (byte) 0);
                        result = new Result(USER_CANCELED, null);
                    }
                    // End this prompt's owner watch atomically with delivery. A first
                    // enrollment signature captures and watches its own current owner.
                    // Its later binding-file commit still owns the native create slot.
                    if (retainCreation && result.status == SUCCESS) {
                        mDelivered = true;
                    } else {
                        disposeCreation(result.status == SUCCESS);
                    }
                }
                return result;
            } finally {
                synchronized (this) {
                    if (!mDelivered) disposeCreation(false);
                }
            }
        }

        synchronized void disposeCreation(boolean accepted) {
            if (mReleased) return;
            mReleased = true;
            mRelease.accept(accepted);
        }

        @Override
        public void cancel() {
            synchronized (this) {
                if (mReleased || mDelivered || mCanceled) return;
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
