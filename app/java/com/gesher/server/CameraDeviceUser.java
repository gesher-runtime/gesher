/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.hardware.ICameraService;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CameraMetadataInfo;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.ICameraDeviceCallbacks;
import android.hardware.camera2.ICameraDeviceUser;
import android.hardware.camera2.impl.CameraMetadataNative;
import android.hardware.camera2.impl.CaptureResultExtras;
import android.hardware.camera2.impl.PhysicalCaptureResultInfo;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.utils.SubmitInfo;
import android.hardware.camera2.utils.SurfaceUtils;
import android.hardware.common.fmq.MQDescriptor;
import android.os.RemoteException;
import android.os.ServiceSpecificException;
import android.os.SystemClock;
import android.util.Size;
import android.view.Surface;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * An open camera (cameraserver's CameraDeviceClient): streams, requests and a capture
 * thread that reads frames from {@link WindowsCamera} and hands each request's targets
 * their frame ({@link CameraFrames}), then reports onCaptureStarted and the result.
 * Single captures go before the repeating request; frame numbers count up in the order
 * frames are made (a burst's at submission, so SubmitInfo can tell its last one).
 */
public final class CameraDeviceUser extends ICameraDeviceUser.Default {
    /** One configured output. */
    private static final class Stream {
        final List<Surface> surfaces = new ArrayList<>();
        int format;  // the HAL format of its first surface
        Size size;
    }

    /** One frame to make: a request's copy, its id and its frame number. */
    private static final class Frame {
        final Request request;
        final int subsequenceId;
        long frameNumber;

        Frame(Request request, int subsequenceId, long frameNumber) {
            this.request = request;
            this.subsequenceId = subsequenceId;
            this.frameNumber = frameNumber;
        }
    }

    /** A submitted request, copied (the app may reuse its own). */
    private static final class Request {
        final int requestId;
        final CameraMetadataNative settings;
        final int[] streamIds;
        final int[] surfaceIds;

        Request(int requestId, CaptureRequest request) {
            this.requestId = requestId;
            this.settings = request.getNativeCopy();
            int[] streams = request.getStreamIds();
            int[] surfaces = request.getSurfaceIds();
            this.streamIds = streams != null ? streams.clone() : new int[0];
            this.surfaceIds = surfaces != null ? surfaces.clone() : new int[0];
        }
    }

    private final CameraService mService;
    private final String mCameraId;
    private final WindowsCamera.Device mDevice;
    private final ICameraDeviceCallbacks mCallbacks;

    private final Object mLock = new Object();
    private final Map<Integer, Stream> mStreams = new HashMap<>();
    private final ArrayDeque<Frame> mQueue = new ArrayDeque<>();
    private Request[] mRepeating;
    private int mRepeatingId = -1;
    private long mRepeatingLastFrame = NO_IN_FLIGHT_REPEATING_FRAMES;
    private int mNextStreamId;
    private int mNextRequestId;
    private long mNextFrameNumber;
    private boolean mBusy;
    private boolean mIdleReported = true;
    private boolean mDisconnected;
    private Thread mThread;
    private WindowsCamera.Capture mCapture;

    CameraDeviceUser(CameraService service, String cameraId, WindowsCamera.Device device,
            ICameraDeviceCallbacks callbacks) {
        mService = service;
        mCameraId = cameraId;
        mDevice = device;
        mCallbacks = callbacks;
    }

    // --- configuration -----------------------------------------------------------------------

    @Override
    public void beginConfigure() {
        checkConnected();
    }

    @Override
    public int[] endConfigure(int operatingMode, CameraMetadataNative sessionParams,
            long startTimeMs) {
        checkConnected();
        if (operatingMode != NORMAL_MODE) {
            throw new ServiceSpecificException(ICameraService.ERROR_ILLEGAL_ARGUMENT,
                    "operating mode " + operatingMode + " is not supported");
        }
        return new int[0];
    }

    @Override
    public boolean isSessionConfigurationSupported(SessionConfiguration sessionConfiguration) {
        return true;
    }

    @Override
    public int createStream(OutputConfiguration config) {
        checkConnected();
        List<Surface> surfaces = config.getSurfaces();
        if (surfaces.isEmpty()) {
            throw new ServiceSpecificException(ICameraService.ERROR_ILLEGAL_ARGUMENT,
                    "an output with no surface (deferred surfaces are not supported)");
        }
        Stream s = new Stream();
        s.surfaces.addAll(surfaces);
        s.format = SurfaceUtils.getSurfaceFormat(surfaces.get(0));
        s.size = SurfaceUtils.getSurfaceSize(surfaces.get(0));
        synchronized (mLock) {
            int id = mNextStreamId++;
            mStreams.put(id, s);
            CameraService.log("createStream " + id + ": format 0x" + Integer.toHexString(s.format)
                    + ", " + s.size + (CameraFrames.isReader(surfaces.get(0)) ? ", an ImageReader"
                    : ", a window surface"));
            return id;
        }
    }

    @Override
    public void deleteStream(int streamId) {
        synchronized (mLock) {
            mStreams.remove(streamId);
        }
    }

    @Override
    public void updateOutputConfiguration(int streamId, OutputConfiguration config) {
        synchronized (mLock) {
            Stream s = mStreams.get(streamId);
            if (s == null) return;
            s.surfaces.clear();
            s.surfaces.addAll(config.getSurfaces());
        }
    }

    @Override
    public void finalizeOutputConfigurations(int streamId, OutputConfiguration config) {
        updateOutputConfiguration(streamId, config);
    }

    @Override
    public int createInputStream(int width, int height, int format, boolean isMultiResolution) {
        throw new ServiceSpecificException(ICameraService.ERROR_ILLEGAL_ARGUMENT,
                "reprocessing is not supported");
    }

    @Override
    public void prepare(int streamId) {
        try {
            mCallbacks.onPrepared(streamId);
        } catch (RemoteException e) {
            // gone
        }
    }

    @Override
    public void prepare2(int maxCount, int streamId) {
        prepare(streamId);
    }

    @Override
    public void tearDown(int streamId) {
    }

    @Override
    public CameraMetadataNative createDefaultRequest(int templateId) {
        checkConnected();
        return CameraService.defaultRequest(mDevice, templateId);
    }

    @Override
    public CameraMetadataNative getCameraInfo() {
        return CameraService.characteristics(mDevice);
    }

    @Override
    public MQDescriptor getCaptureResultMetadataQueue() {
        // No fast message queue: results come in the callbacks.
        return new MQDescriptor();
    }

    @Override
    public boolean isPrimaryClient() {
        return true;
    }

    @Override
    public int getGlobalAudioRestriction() {
        return AUDIO_RESTRICTION_NONE;
    }

    @Override
    public void setCameraAudioRestriction(int mode) {
    }

    // --- requests ------------------------------------------------------------------------------

    @Override
    public SubmitInfo submitRequest(CaptureRequest request, boolean streaming) {
        return submitRequestList(new CaptureRequest[] {request}, streaming);
    }

    @Override
    public SubmitInfo submitRequestList(CaptureRequest[] requests, boolean streaming) {
        checkConnected();
        if (requests == null || requests.length == 0) {
            throw new ServiceSpecificException(ICameraService.ERROR_ILLEGAL_ARGUMENT,
                    "no requests");
        }
        synchronized (mLock) {
            int requestId = mNextRequestId++;
            Request[] copies = new Request[requests.length];
            for (int i = 0; i < requests.length; i++) {
                copies[i] = new Request(requestId, requests[i]);
                for (int streamId : copies[i].streamIds) {
                    if (!mStreams.containsKey(streamId)) {
                        throw new ServiceSpecificException(ICameraService.ERROR_ILLEGAL_ARGUMENT,
                                "request targets stream " + streamId + ", not configured");
                    }
                }
            }
            SubmitInfo info;
            if (streaming) {
                info = new SubmitInfo(requestId, mRepeatingLastFrame);
                mRepeating = copies;
                mRepeatingId = requestId;
                mRepeatingLastFrame = NO_IN_FLIGHT_REPEATING_FRAMES;
            } else {
                long last = -1;
                for (int i = 0; i < copies.length; i++) {
                    last = mNextFrameNumber++;
                    mQueue.add(new Frame(copies[i], i, last));
                }
                info = new SubmitInfo(requestId, last);
            }
            startThreadLocked();
            mIdleReported = false;
            mLock.notifyAll();
            return info;
        }
    }

    @Override
    public long cancelRequest(int requestId) {
        synchronized (mLock) {
            if (requestId != mRepeatingId || mRepeating == null) {
                throw new ServiceSpecificException(ICameraService.ERROR_ILLEGAL_ARGUMENT,
                        "request " + requestId + " is not the repeating request");
            }
            mRepeating = null;
            mRepeatingId = -1;
            long last = mRepeatingLastFrame;
            mRepeatingLastFrame = NO_IN_FLIGHT_REPEATING_FRAMES;
            mLock.notifyAll();
            return last;
        }
    }

    @Override
    public long flush() {
        synchronized (mLock) {
            mRepeating = null;
            mRepeatingId = -1;
            mRepeatingLastFrame = NO_IN_FLIGHT_REPEATING_FRAMES;
            mLock.notifyAll();
            return mNextFrameNumber - 1;
        }
    }

    @Override
    public void waitUntilIdle() {
        synchronized (mLock) {
            if (mRepeating != null) {
                throw new ServiceSpecificException(ICameraService.ERROR_INVALID_OPERATION,
                        "a repeating request is active");
            }
            long deadline = SystemClock.uptimeMillis() + 10_000;
            while ((!mQueue.isEmpty() || mBusy) && !mDisconnected) {
                long left = deadline - SystemClock.uptimeMillis();
                if (left <= 0) {
                    throw new ServiceSpecificException(ICameraService.ERROR_INVALID_OPERATION,
                            "timed out waiting for the camera to be idle");
                }
                try {
                    mLock.wait(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }
    }

    @Override
    public void disconnect() {
        Thread thread;
        synchronized (mLock) {
            if (mDisconnected) return;
            mDisconnected = true;
            mRepeating = null;
            mQueue.clear();
            thread = mThread;
            mLock.notifyAll();
        }
        WindowsCamera.Capture capture = mCapture;
        if (thread != null && thread != Thread.currentThread()) {
            try {
                thread.join(5000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        if (capture != null) capture.close();
        mService.closed(mCameraId, this);
        CameraService.log("disconnect " + mCameraId);
    }

    /** Another client opened this camera. */
    void evicted() {
        try {
            mCallbacks.onDeviceError(ICameraDeviceCallbacks.ERROR_CAMERA_DISCONNECTED,
                    extras(-1, 0, -1));
        } catch (RemoteException e) {
            // gone
        }
        disconnect();
    }

    private void checkConnected() {
        synchronized (mLock) {
            if (mDisconnected) {
                throw new ServiceSpecificException(ICameraService.ERROR_DISCONNECTED,
                        "camera " + mCameraId + " is disconnected");
            }
        }
    }

    // --- the capture thread --------------------------------------------------------------------

    private void startThreadLocked() {
        if (mThread != null) return;
        mThread = new Thread(this::captureLoop, "camera-" + mCameraId);
        mThread.setDaemon(true);
        mThread.start();
    }

    /** The next frame to make, or null when disconnected; reports idleness. */
    private Frame next() {
        boolean reportIdle = false;
        synchronized (mLock) {
            mBusy = false;
            mLock.notifyAll();
            while (!mDisconnected && mQueue.isEmpty() && mRepeating == null) {
                if (!mIdleReported) {
                    mIdleReported = true;
                    reportIdle = true;
                    break;
                }
                try {
                    mLock.wait();
                } catch (InterruptedException e) {
                    return null;
                }
            }
        }
        if (reportIdle) {
            try {
                mCallbacks.onDeviceIdle();
            } catch (RemoteException e) {
                // gone
            }
            return next();
        }
        synchronized (mLock) {
            if (mDisconnected) return null;
            mBusy = true;
            Frame f = mQueue.poll();
            if (f != null) return f;
            // The repeating request's next frame (its first request, for a burst).
            Request r = mRepeating[0];
            f = new Frame(r, 0, mNextFrameNumber++);
            mRepeatingLastFrame = f.frameNumber;
            return f;
        }
    }

    private void captureLoop() {
        byte[] nv12 = null;
        Frame f;
        while ((f = next()) != null) {
            List<Stream> targets = new ArrayList<>();
            synchronized (mLock) {
                for (int streamId : f.request.streamIds) {
                    Stream s = mStreams.get(streamId);
                    if (s != null) targets.add(s);
                }
            }
            if (mCapture == null) {
                try {
                    mCapture = open(targets);
                } catch (WindowsCamera.OpenException e) {
                    CameraService.log(e.getMessage());
                    fail(f, ICameraDeviceCallbacks.ERROR_CAMERA_DEVICE);
                    continue;
                }
            }
            if (nv12 == null || nv12.length < mCapture.width * mCapture.height * 3 / 2) {
                nv12 = new byte[mCapture.width * mCapture.height * 3 / 2];
            }
            int hr = mCapture.read(nv12);
            long timestamp = SystemClock.elapsedRealtimeNanos();
            if (hr < 0) {
                CameraService.log(String.format("reading camera %s: HRESULT 0x%08X", mCameraId,
                        hr));
                fail(f, ICameraDeviceCallbacks.ERROR_CAMERA_DEVICE);
                continue;
            }
            CaptureResultExtras extras = extras(f.request.requestId, f.subsequenceId,
                    f.frameNumber);
            try {
                mCallbacks.onCaptureStarted(extras, timestamp);
                for (int i = 0; i < f.request.streamIds.length; i++) {
                    Stream s;
                    synchronized (mLock) {
                        s = mStreams.get(f.request.streamIds[i]);
                    }
                    if (s == null) continue;
                    int surfaceId = i < f.request.surfaceIds.length ? f.request.surfaceIds[i] : 0;
                    Surface surface = surfaceId < s.surfaces.size() ? s.surfaces.get(surfaceId)
                            : s.surfaces.get(0);
                    byte[] jpeg = null;
                    if (s.format == CameraService.HAL_BLOB) {
                        Byte quality = f.request.settings.get(CaptureRequest.JPEG_QUALITY);
                        jpeg = CameraFrames.jpeg(nv12, mCapture.width, mCapture.height,
                                s.size.getWidth(), s.size.getHeight(),
                                quality != null ? quality & 0xff : 95);
                    }
                    if (CameraFrames.deliver(surface, nv12, mCapture.width, mCapture.height,
                            timestamp, jpeg) != CameraFrames.OK) {
                        mCallbacks.onDeviceError(ICameraDeviceCallbacks.ERROR_CAMERA_BUFFER,
                                extras(f.request.requestId, f.subsequenceId, f.frameNumber,
                                        f.request.streamIds[i]));
                    }
                }
                CameraMetadataNative result = result(f.request.settings, timestamp);
                mCallbacks.onResultReceived(CameraMetadataInfo.metadata(result), extras,
                        new PhysicalCaptureResultInfo[0]);
            } catch (RemoteException | RuntimeException e) {
                CameraService.log("frame " + f.frameNumber + " of camera " + mCameraId
                        + " failed: " + e);
            }
        }
    }

    /** Opens the device at the largest size a target asks for. */
    private WindowsCamera.Capture open(List<Stream> targets) throws WindowsCamera.OpenException {
        int w = 640, h = 480;
        long area = 0;
        synchronized (mLock) {
            for (Stream s : mStreams.values()) {
                long a = (long) s.size.getWidth() * s.size.getHeight();
                if (a > area) {
                    area = a;
                    w = s.size.getWidth();
                    h = s.size.getHeight();
                }
            }
        }
        WindowsCamera.Capture capture = WindowsCamera.open(mDevice.id, w, h);
        CameraService.log("camera " + mCameraId + " (" + mDevice.name + ") capturing at "
                + capture.width + "x" + capture.height);
        return capture;
    }

    private void fail(Frame f, int error) {
        try {
            mCallbacks.onDeviceError(error, extras(f.request.requestId, f.subsequenceId,
                    f.frameNumber));
        } catch (RemoteException e) {
            // gone
        }
        synchronized (mLock) {
            if (error == ICameraDeviceCallbacks.ERROR_CAMERA_DEVICE && mRepeating != null) {
                long last = mRepeatingLastFrame;
                int id = mRepeatingId;
                mRepeating = null;
                mRepeatingId = -1;
                try {
                    mCallbacks.onRepeatingRequestError(last, id);
                } catch (RemoteException e) {
                    // gone
                }
            }
        }
    }

    static CaptureResultExtras extras(int requestId, int subsequenceId, long frameNumber) {
        return extras(requestId, subsequenceId, frameNumber, -1);
    }

    static CaptureResultExtras extras(int requestId, int subsequenceId, long frameNumber,
            int errorStreamId) {
        return new CaptureResultExtras(requestId, subsequenceId, 0 /* afTriggerId */,
                0 /* precaptureTriggerId */, frameNumber, 1 /* partialResultCount */,
                errorStreamId, null, frameNumber /* lastCompletedRegularFrameNumber */,
                -1 /* lastCompletedReprocessFrameNumber */, -1 /* lastCompletedZslFrameNumber */,
                false, 0);
    }

    /** The result of a frame: the request's settings and what the camera did. */
    static CameraMetadataNative result(CameraMetadataNative settings, long timestamp) {
        CameraMetadataNative m = new CameraMetadataNative(settings);
        m.set(CaptureResult.SENSOR_TIMESTAMP, timestamp);
        m.set(CaptureResult.SENSOR_EXPOSURE_TIME, CameraService.FRAME_DURATION_NS);
        m.set(CaptureResult.SENSOR_FRAME_DURATION, CameraService.FRAME_DURATION_NS);
        m.set(CaptureResult.REQUEST_PIPELINE_DEPTH, (byte) 1);
        m.set(CaptureResult.CONTROL_AE_STATE, CameraMetadata.CONTROL_AE_STATE_CONVERGED);
        m.set(CaptureResult.CONTROL_AF_STATE, CameraMetadata.CONTROL_AF_STATE_INACTIVE);
        m.set(CaptureResult.CONTROL_AWB_STATE, CameraMetadata.CONTROL_AWB_STATE_CONVERGED);
        m.set(CaptureResult.FLASH_STATE, CameraMetadata.FLASH_STATE_UNAVAILABLE);
        m.set(CaptureResult.LENS_STATE, CameraMetadata.LENS_STATE_STATIONARY);
        return m;
    }
}
