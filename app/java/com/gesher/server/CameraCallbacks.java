/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.hardware.ICameraService;
import android.hardware.camera2.CameraMetadataInfo;
import android.hardware.camera2.ICameraDeviceCallbacks;
import android.hardware.camera2.impl.CameraMetadataNative;
import android.hardware.camera2.impl.CaptureResultExtras;
import android.hardware.camera2.impl.PhysicalCaptureResultInfo;
import android.os.RemoteException;
import android.os.ServiceSpecificException;

import java.util.ArrayDeque;

/** Ordered callbacks whose admission includes queued, producing and executing work. */
final class CameraCallbacks implements AutoCloseable {
    static final int MAX_FRAMES = 64;
    static final int MAX_TARGETS = 64;
    static final long MAX_METADATA_BYTES = 256 * 1024;
    static final long MAX_BYTES = 8 * 1024 * 1024;
    static final int MAX_CONTROLS = 16;
    // A closed queue with an uncooperative client still owns its slot. Reopening
    // cameras cannot create an unlimited number of blocked callback workers.
    static final int MAX_QUEUES = 8;
    private static int sQueues;

    private interface Call { void run() throws RemoteException; }

    private final String mCameraId;
    private final ICameraDeviceCallbacks mTarget;
    private final Runnable mCapacityChanged;
    private final ArrayDeque<Task> mQueue = new ArrayDeque<>();
    private final Object mLock = new Object();
    private int mFrames, mControls, mLeases;
    private long mBytes;
    private boolean mIdlePending, mDeviceErrorPosted, mClosed, mReleased;
    private Thread mWorker;

    private static final class Task {
        final Call call;
        final FrameCallbacks frame;
        final int kind; // 0 frame/device error, 1 external control, 2 idle
        Task(Call call, FrameCallbacks frame, int kind) {
            this.call = call;
            this.frame = frame;
            this.kind = kind;
        }
    }

    CameraCallbacks(String cameraId, ICameraDeviceCallbacks target, Runnable capacityChanged) {
        synchronized (CameraCallbacks.class) {
            if (sQueues == MAX_QUEUES) {
                throw new ServiceSpecificException(ICameraService.ERROR_MAX_CAMERAS_IN_USE,
                        "camera callback resources are still in use");
            }
            ++sQueues;
        }
        mCameraId = cameraId;
        mTarget = target;
        mCapacityChanged = capacityChanged;
    }

    static ServiceSpecificException full() {
        // CameraDeviceClient also reports ERROR_INVALID_OPERATION when captureList
        // cannot admit a submission. No request/frame ID has been committed yet.
        return new ServiceSpecificException(ICameraService.ERROR_INVALID_OPERATION,
                "camera request/callback capacity is in use");
    }

    /** Native allocated capacity, not just the number of metadata entries. */
    static long metadataBytes(CameraMetadataNative metadata) {
        synchronized (metadata) {
            long bytes = nativeMetadataBytes(metadata.getMetadataPtr());
            if (bytes <= 0 || bytes > MAX_METADATA_BYTES) throw full();
            return bytes;
        }
    }

    private static native long nativeMetadataBytes(long metadata);

    /** Retains the repeating template independently of its in-flight frames. */
    final class Retained implements AutoCloseable {
        private final long bytes;
        private boolean live = true;
        Retained(long bytes) { this.bytes = bytes; }
        @Override public void close() {
            synchronized (mLock) {
                if (!live) return;
                live = false;
                mBytes -= bytes;
                --mLeases;
                releasedLocked();
            }
            mCapacityChanged.run();
        }
    }

    Retained retain(long bytes) {
        synchronized (mLock) {
            if (mClosed || bytes < 0 || bytes > MAX_BYTES - mBytes) return null;
            mBytes += bytes;
            ++mLeases;
            return new Retained(bytes);
        }
    }

    boolean hasFrameCapacity(int count) {
        synchronized (mLock) {
            return !mClosed && count <= MAX_FRAMES - mFrames;
        }
    }

    /** Atomically admit a complete burst; the caller cannot leave a partial burst. */
    FrameCallbacks[] reserve(long[] bytes) {
        synchronized (mLock) {
            if (mClosed || bytes.length > MAX_FRAMES - mFrames) return null;
            long total = 0;
            for (long n : bytes) {
                if (n < 0 || n > MAX_BYTES - mBytes - total) return null;
                total += n;
            }
            FrameCallbacks[] frames = new FrameCallbacks[bytes.length];
            for (int i = 0; i < bytes.length; ++i) frames[i] = new FrameCallbacks(bytes[i]);
            mFrames += frames.length;
            mBytes += total;
            return frames;
        }
    }

    /** A frame owns its credits until its producer AND every callback have finished. */
    final class FrameCallbacks extends ICameraDeviceCallbacks.Default implements AutoCloseable {
        private final long bytes;
        private int pending;
        private boolean terminal, sealed, released;

        FrameCallbacks(long bytes) { this.bytes = bytes; }

        private void post(Call call, boolean last) {
            synchronized (mLock) {
                if (sealed || terminal) return; // a flush/disconnect already terminated this frame
                if (last) terminal = true;
                ++pending;
                enqueueLocked(new Task(call, this, 0));
            }
        }

        @Override public void onCaptureStarted(CaptureResultExtras extras, long timestamp) {
            post(() -> mTarget.onCaptureStarted(extras, timestamp), false);
        }

        @Override public void onResultReceived(CameraMetadataInfo metadata, CaptureResultExtras extras,
                PhysicalCaptureResultInfo[] physical) {
            post(() -> mTarget.onResultReceived(metadata, extras, physical), true);
        }

        @Override public void onDeviceError(int error, CaptureResultExtras extras) {
            post(() -> mTarget.onDeviceError(error, extras), error != ERROR_CAMERA_BUFFER);
        }

        @Override public void onRepeatingRequestError(long lastFrame, int requestId) {
            synchronized (mLock) {
                if (sealed) return;
                ++pending;
                enqueueLocked(new Task(() -> mTarget.onRepeatingRequestError(lastFrame, requestId),
                        this, 0));
            }
        }

        boolean terminal() { synchronized (mLock) { return terminal; } }

        @Override public void close() {
            synchronized (mLock) {
                sealed = true;
                releaseIfDoneLocked();
                releasedLocked();
            }
            mCapacityChanged.run();
        }

        private void releaseIfDoneLocked() {
            if (sealed && pending == 0 && !released) {
                released = true;
                --mFrames;
                mBytes -= bytes;
            }
        }
    }

    /** External control methods never block a caller holding CameraDeviceImpl's lock. */
    void prepared(int streamId) {
        synchronized (mLock) {
            if (mClosed || mControls == MAX_CONTROLS) throw full();
            ++mControls;
            enqueueLocked(new Task(() -> mTarget.onPrepared(streamId), null, 1));
        }
    }

    /** The capture producer may wait for this dedicated state-callback slot. */
    boolean idle() {
        synchronized (mLock) {
            if (mClosed) return true;
            if (mIdlePending) return false;
            mIdlePending = true;
            enqueueLocked(new Task(() -> mTarget.onDeviceIdle(), null, 2));
            return true;
        }
    }

    /** One reserved terminal event per camera, independent of frame/control saturation. */
    void disconnected(CaptureResultExtras extras) {
        synchronized (mLock) {
            if (mDeviceErrorPosted || mClosed) return;
            mDeviceErrorPosted = true;
            enqueueLocked(new Task(() -> mTarget.onDeviceError(
                    ICameraDeviceCallbacks.ERROR_CAMERA_DISCONNECTED, extras), null, 0));
        }
    }

    private void enqueueLocked(Task task) {
        mQueue.add(task);
        if (mWorker == null) {
            mWorker = new Thread(this::drain, "camera-callback-" + mCameraId);
            mWorker.setDaemon(true);
            mWorker.start();
        }
        mLock.notifyAll();
    }

    private void drain() {
        for (;;) {
            Task task;
            synchronized (mLock) {
                while (mQueue.isEmpty()) {
                    if (mClosed && mFrames == 0 && mLeases == 0) {
                        mWorker = null;
                        releaseQueueLocked();
                        return;
                    }
                    try { mLock.wait(); }
                    catch (InterruptedException ignored) {
                        // Interrupt is not permission to lose admitted completions.
                    }
                }
                task = mQueue.remove();
            }
            try {
                task.call.run();
            } catch (Throwable e) {
                // A local application can also throw Error. Do not strand the
                // bounded worker's already admitted queue behind that failure.
                CameraService.log("camera " + mCameraId + " callback failed: " + e);
            } finally {
                synchronized (mLock) {
                    if (task.frame != null) {
                        --task.frame.pending;
                        task.frame.releaseIfDoneLocked();
                    }
                    if (task.kind == 1) --mControls;
                    if (task.kind == 2) mIdlePending = false;
                    releasedLocked();
                }
                // Never enter the camera lock while holding the callback queue lock.
                mCapacityChanged.run();
            }
            task = null; // do not retain the last result while the worker is idle
        }
    }

    private void releasedLocked() {
        mLock.notifyAll();
        if (mClosed && mWorker == null && mFrames == 0 && mLeases == 0) releaseQueueLocked();
    }

    private void releaseQueueLocked() {
        if (!mReleased) {
            mReleased = true;
            synchronized (CameraCallbacks.class) { --sQueues; }
        }
    }

    @Override public void close() {
        synchronized (mLock) {
            mClosed = true;
            releasedLocked();
        }
        mCapacityChanged.run();
        // Never join: a reentrant close may hold the framework lock needed by a callback.
        // Neither a blocked callback nor a native read is declared forcibly terminated.
    }
}
