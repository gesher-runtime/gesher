/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devices;

import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.provider.MediaStore;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Camera2 end to end: the id list, characteristics, openCamera, a capture session with a
 * repeating preview into a YUV ImageReader and a still capture into a JPEG ImageReader,
 * then closing. With GESHER_CAMERA_TEST the test camera's frames are known (Y = x + y away
 * from a moving square). Also the legacy android.hardware.Camera and ACTION_IMAGE_CAPTURE.
 */
final class CameraSteps {
    private final Context mContext;
    private final CameraManager mCm;
    private final HandlerThread mThread = new HandlerThread("camera-callbacks");
    private final Handler mHandler;
    private final Executor mExecutor;
    private final BlockingQueue<String> mEvents = new LinkedBlockingQueue<>();

    CameraSteps(Context context) {
        mContext = context;
        mCm = context.getSystemService(CameraManager.class);
        mThread.start();
        mHandler = new Handler(mThread.getLooper());
        mExecutor = mHandler::post;
    }

    /** Events that came before the one waited for (a still's image comes before its result). */
    private final List<String> mEarlier = new java.util.ArrayList<>();

    private String next(String prefix, long timeoutMs) throws InterruptedException {
        for (int i = 0; i < mEarlier.size(); i++) {
            if (mEarlier.get(i).startsWith(prefix)) return mEarlier.remove(i);
        }
        long deadline = System.currentTimeMillis() + timeoutMs;
        for (;;) {
            long left = deadline - System.currentTimeMillis();
            String e = left > 0 ? mEvents.poll(left, TimeUnit.MILLISECONDS) : null;
            if (e == null) return "(no " + prefix + " within " + timeoutMs + " ms)";
            if (e.startsWith(prefix)) return e;
            mEarlier.add(e);
        }
    }

    void run() throws Exception {
        String[] ids = mCm.getCameraIdList();
        Report.line("camera: " + ids.length + " cameras" + (ids.length > 0 ? ": "
                + String.join(", ", ids) : ""));
        legacyAndIntent();
        if (ids.length == 0) {
            try {
                mCm.openCamera("0", new CameraDevice.StateCallback() {
                    @Override
                    public void onOpened(CameraDevice camera) {
                        camera.close();
                    }

                    @Override
                    public void onDisconnected(CameraDevice camera) {
                    }

                    @Override
                    public void onError(CameraDevice camera, int error) {
                    }
                }, mHandler);
                Report.line("camera: openCamera(\"0\") opened a camera that is not there");
            } catch (IllegalArgumentException | CameraAccessException e) {
                Report.line("camera: no cameras; openCamera(\"0\") throws "
                        + e.getClass().getSimpleName());
            }
            Report.line("camera: done");
            mThread.quitSafely();
            return;
        }
        String id = ids[ids.length - 1];  // the test camera comes last
        CameraCharacteristics c = mCm.getCameraCharacteristics(id);
        StreamConfigurationMap map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Report.line("camera: " + id + " facing " + c.get(CameraCharacteristics.LENS_FACING)
                + ", level " + c.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)
                + ", YUV_420_888 " + Arrays.toString(map.getOutputSizes(ImageFormat.YUV_420_888))
                + ", JPEG " + Arrays.toString(map.getOutputSizes(ImageFormat.JPEG)));

        // The outputs: a preview into a YUV reader, stills into a JPEG reader (smaller: scaled).
        ImageReader preview = ImageReader.newInstance(640, 480, ImageFormat.YUV_420_888, 4);
        ImageReader still = ImageReader.newInstance(320, 240, ImageFormat.JPEG, 2);
        AtomicInteger previews = new AtomicInteger();
        preview.setOnImageAvailableListener(r -> {
            Image image = r.acquireLatestImage();
            if (image == null) return;
            if (previews.incrementAndGet() == 1) {
                Image.Plane y = image.getPlanes()[0];
                ByteBuffer b = y.getBuffer();
                int at = 100 * y.getRowStride() + 10;
                mEvents.add("preview " + image.getWidth() + "x" + image.getHeight() + " format "
                        + image.getFormat() + ", " + image.getPlanes().length + " planes, Y(10,100)="
                        + (b.get(at) & 0xff) + ", timestamp>0 " + (image.getTimestamp() > 0));
            }
            image.close();
        }, mHandler);
        still.setOnImageAvailableListener(r -> {
          try {
            Image image = r.acquireNextImage();
            if (image == null) return;
            ByteBuffer b = image.getPlanes()[0].getBuffer();
            byte[] jpeg = new byte[b.remaining()];
            b.get(jpeg);
            Bitmap bitmap = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
            mEvents.add("still JPEG " + image.getWidth() + "x" + image.getHeight()
                    + ", starts FFD8 " + (jpeg.length > 2 && (jpeg[0] & 0xff) == 0xff
                    && (jpeg[1] & 0xff) == 0xd8) + ", decodes to " + (bitmap == null ? "nothing"
                    : bitmap.getWidth() + "x" + bitmap.getHeight()));
            image.close();
          } catch (RuntimeException e) {
            mEvents.add("still image failed: " + e);
          }
        }, mHandler);

        CameraDevice[] device = new CameraDevice[1];
        mCm.openCamera(id, mExecutor, new CameraDevice.StateCallback() {
            @Override
            public void onOpened(CameraDevice camera) {
                device[0] = camera;
                mEvents.add("opened " + camera.getId());
            }

            @Override
            public void onClosed(CameraDevice camera) {
                mEvents.add("closed " + camera.getId());
            }

            @Override
            public void onDisconnected(CameraDevice camera) {
                mEvents.add("disconnected");
            }

            @Override
            public void onError(CameraDevice camera, int error) {
                mEvents.add("error " + error);
            }
        });
        Report.line("camera: openCamera " + next("opened", 10000));
        if (device[0] == null) {
            Report.line("camera: done");
            return;
        }
        CameraCaptureSession[] session = new CameraCaptureSession[1];
        device[0].createCaptureSession(new SessionConfiguration(
                SessionConfiguration.SESSION_REGULAR,
                List.of(new OutputConfiguration(preview.getSurface()),
                        new OutputConfiguration(still.getSurface())),
                mExecutor, new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession s) {
                        session[0] = s;
                        mEvents.add("configured");
                    }

                    @Override
                    public void onConfigureFailed(CameraCaptureSession s) {
                        mEvents.add("configure failed");
                    }
                }));
        Report.line("camera: createCaptureSession " + next("configure", 10000));
        if (session[0] == null) {
            device[0].close();
            Report.line("camera: done");
            return;
        }
        AtomicInteger completed = new AtomicInteger();
        CameraCaptureSession.CaptureCallback counting = new CameraCaptureSession.CaptureCallback() {
            @Override
            public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest request,
                    TotalCaptureResult result) {
                completed.incrementAndGet();
            }
        };
        CaptureRequest.Builder previewRequest =
                device[0].createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
        previewRequest.addTarget(preview.getSurface());
        session[0].setRepeatingRequest(previewRequest.build(), counting, mHandler);
        Report.line("camera: repeating preview: first frame " + next("preview", 10000));
        Thread.sleep(1000);
        Report.line("camera: preview frames keep coming: " + (previews.get() >= 5) + ", results "
                + (completed.get() >= 5));

        CaptureRequest.Builder stillRequest =
                device[0].createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
        stillRequest.addTarget(still.getSurface());
        stillRequest.set(CaptureRequest.JPEG_QUALITY, (byte) 90);
        session[0].capture(stillRequest.build(), new CameraCaptureSession.CaptureCallback() {
            @Override
            public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest request,
                    TotalCaptureResult result) {
                mEvents.add("capture completed, sensor timestamp>0 "
                        + (result.get(android.hardware.camera2.CaptureResult.SENSOR_TIMESTAMP)
                                > 0) + ", JPEG quality "
                        + result.get(android.hardware.camera2.CaptureResult.JPEG_QUALITY));
            }

            @Override
            public void onCaptureFailed(CameraCaptureSession s, CaptureRequest request,
                    android.hardware.camera2.CaptureFailure failure) {
                mEvents.add("still capture failed, reason " + failure.getReason());
            }

            @Override
            public void onCaptureBufferLost(CameraCaptureSession s, CaptureRequest request,
                    android.view.Surface target, long frameNumber) {
                mEvents.add("still buffer lost (frame " + frameNumber + ")");
            }
        }, mHandler);
        Report.line("camera: still capture: " + next("capture completed", 10000) + "; "
                + next("still", 10000));
        session[0].stopRepeating();
        session[0].close();
        device[0].close();
        Report.line("camera: close " + next("closed", 10000));
        preview.close();
        still.close();
        Report.line("camera: done");
        mThread.quitSafely();
    }

    @SuppressWarnings("deprecation")
    private void legacyAndIntent() {
        Report.line("camera: legacy android.hardware.Camera.getNumberOfCameras() "
                + android.hardware.Camera.getNumberOfCameras());
        try {
            android.hardware.Camera legacy = android.hardware.Camera.open();
            Report.line("camera: legacy Camera.open() answers " + (legacy == null ? "null"
                    : "a camera"));
        } catch (RuntimeException e) {
            Report.line("camera: legacy Camera.open() throws " + e.getClass().getSimpleName());
        }
        Intent capture = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
        Report.line("camera: ACTION_IMAGE_CAPTURE resolves to "
                + mContext.getPackageManager().resolveActivity(capture, 0));
    }
}
