/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.hardware.camera2.CameraMetadataInfo;
import android.hardware.camera2.ICameraDeviceCallbacks;
import android.hardware.camera2.impl.CaptureResultExtras;
import android.hardware.camera2.impl.PhysicalCaptureResultInfo;
import android.os.IBinder;
import android.os.RemoteException;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;

/** The camera's ordered one-way callbacks, including when its client is local. */
final class CameraCallbacks extends ICameraDeviceCallbacks.Default implements AutoCloseable {
    private interface Call { void run() throws RemoteException; }

    private final String mCameraId;
    private final ICameraDeviceCallbacks mTarget;
    private final ExecutorService mExecutor;

    CameraCallbacks(String cameraId, ICameraDeviceCallbacks target) {
        mCameraId = cameraId;
        mTarget = target;
        mExecutor = Executors.newSingleThreadExecutor(task -> {
            Thread thread = new Thread(task, "camera-callback-" + cameraId);
            thread.setDaemon(true);
            return thread;
        });
    }

    private void post(Call call) {
        try {
            mExecutor.execute(() -> {
                try {
                    call.run();
                } catch (RemoteException | RuntimeException e) {
                    CameraService.log("camera " + mCameraId + " callback failed: " + e);
                }
            });
        } catch (RejectedExecutionException closed) {
            // disconnect already stopped this camera's callbacks.
        }
    }

    @Override
    public IBinder asBinder() { return mTarget.asBinder(); }

    @Override
    public void onDeviceError(int error, CaptureResultExtras extras) {
        post(() -> mTarget.onDeviceError(error, extras));
    }

    @Override
    public void onDeviceIdle() { post(() -> mTarget.onDeviceIdle()); }

    @Override
    public void onCaptureStarted(CaptureResultExtras extras, long timestamp) {
        post(() -> mTarget.onCaptureStarted(extras, timestamp));
    }

    @Override
    public void onResultReceived(CameraMetadataInfo metadata, CaptureResultExtras extras,
            PhysicalCaptureResultInfo[] physical) {
        post(() -> mTarget.onResultReceived(metadata, extras, physical));
    }

    @Override
    public void onPrepared(int streamId) { post(() -> mTarget.onPrepared(streamId)); }

    @Override
    public void onRepeatingRequestError(long lastFrame, int requestId) {
        post(() -> mTarget.onRepeatingRequestError(lastFrame, requestId));
    }

    @Override
    public void onRequestQueueEmpty() { post(() -> mTarget.onRequestQueueEmpty()); }

    @Override
    public void onClientSharedAccessPriorityChanged(boolean primary) {
        post(() -> mTarget.onClientSharedAccessPriorityChanged(primary));
    }

    @Override
    public void close() {
        // Drain already queued callbacks without joining: CameraDeviceImpl.close
        // holds its interface lock while calling disconnect, and those callbacks
        // need that same lock. Their captured arguments remain alive in the queue.
        mExecutor.shutdown();
    }
}
