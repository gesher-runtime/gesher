/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.camera;

import android.app.Activity;
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
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.provider.MediaStore;
import android.util.Size;
import android.view.Gravity;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;

import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.util.List;

/**
 * MediaStore.ACTION_IMAGE_CAPTURE on the Gesher device (hwsvc2 track, ADR 0051): a preview
 * of the camera (Camera2: YUV frames shown in an ImageView, a few a second) with "Take
 * photo" and "Cancel". The photo is a JPEG at the camera's largest size: written to
 * {@link MediaStore#EXTRA_OUTPUT} when the caller gives a Uri (RESULT_OK, no data), else
 * returned as a thumbnail Bitmap in the result's "data" extra, as Android's camera apps do.
 * No camera, or opening it fails: RESULT_CANCELED. {@code com.gesher.camera.extra.AUTO_CAPTURE_MS}
 * (Gesher's own extra) takes the photo by itself after that many milliseconds (tests).
 */
public class CaptureActivity extends Activity {
    static final String EXTRA_AUTO_CAPTURE_MS = "com.gesher.camera.extra.AUTO_CAPTURE_MS";
    private static final int THUMBNAIL_WIDTH = 160;

    private final HandlerThread mThread = new HandlerThread("gesher-camera");
    private Handler mHandler;
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private ImageView mPreview;
    private CameraDevice mDevice;
    private CameraCaptureSession mSession;
    private ImageReader mPreviewReader;
    private ImageReader mStillReader;
    private boolean mDone;
    private long mLastShown;

    static void log(String message) {
        System.out.println("gesher-camera: " + message);
        System.out.flush();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        log("onCreate: " + getIntent() + ", from " + getCallingPackage());
        FrameLayout root = new FrameLayout(this);
        mPreview = new ImageView(this);
        mPreview.setScaleType(ImageView.ScaleType.FIT_CENTER);
        mPreview.setBackgroundColor(0xff000000);
        root.addView(mPreview, new FrameLayout.LayoutParams(-1, -1));
        LinearLayout buttons = new LinearLayout(this);
        buttons.setGravity(Gravity.CENTER);
        Button take = new Button(this);
        take.setText(R.string.take);
        take.setOnClickListener(v -> capture());
        Button cancel = new Button(this);
        cancel.setText(R.string.cancel);
        cancel.setOnClickListener(v -> done(RESULT_CANCELED, null, "cancelled"));
        buttons.addView(take);
        buttons.addView(cancel);
        root.addView(buttons, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
        setContentView(root);

        mThread.start();
        mHandler = new Handler(mThread.getLooper());
        CameraManager cm = getSystemService(CameraManager.class);
        try {
            String[] ids = cm.getCameraIdList();
            if (ids.length == 0) {
                done(RESULT_CANCELED, null, "no camera");
                return;
            }
            open(cm, ids[0]);
        } catch (CameraAccessException | RuntimeException e) {
            done(RESULT_CANCELED, null, "camera: " + e);
            return;
        }
        long auto = getIntent().getLongExtra(EXTRA_AUTO_CAPTURE_MS, -1);
        if (auto >= 0) mMain.postDelayed(this::capture, auto);
    }

    private void open(CameraManager cm, String id) throws CameraAccessException {
        CameraCharacteristics c = cm.getCameraCharacteristics(id);
        StreamConfigurationMap map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size[] jpegSizes = map.getOutputSizes(ImageFormat.JPEG);
        Size largest = jpegSizes[0];
        for (Size s : jpegSizes) {
            if ((long) s.getWidth() * s.getHeight() > (long) largest.getWidth() * largest.getHeight()) {
                largest = s;
            }
        }
        Size[] yuvSizes = map.getOutputSizes(ImageFormat.YUV_420_888);
        Size small = yuvSizes[yuvSizes.length - 1];
        mPreviewReader = ImageReader.newInstance(small.getWidth(), small.getHeight(),
                ImageFormat.YUV_420_888, 3);
        mStillReader = ImageReader.newInstance(largest.getWidth(), largest.getHeight(),
                ImageFormat.JPEG, 2);
        mPreviewReader.setOnImageAvailableListener(this::showPreview, mHandler);
        mStillReader.setOnImageAvailableListener(this::stillTaken, mHandler);
        log("opening camera " + id + ": preview " + small + ", photos " + largest);
        cm.openCamera(id, mHandler::post, new CameraDevice.StateCallback() {
            @Override
            public void onOpened(CameraDevice camera) {
                mDevice = camera;
                startSession();
            }

            @Override
            public void onDisconnected(CameraDevice camera) {
                camera.close();
                done(RESULT_CANCELED, null, "the camera was disconnected");
            }

            @Override
            public void onError(CameraDevice camera, int error) {
                camera.close();
                done(RESULT_CANCELED, null, "camera error " + error);
            }
        });
    }

    private void startSession() {
        try {
            mDevice.createCaptureSession(new SessionConfiguration(SessionConfiguration.SESSION_REGULAR,
                    List.of(new OutputConfiguration(mPreviewReader.getSurface()),
                            new OutputConfiguration(mStillReader.getSurface())),
                    mHandler::post, new CameraCaptureSession.StateCallback() {
                        @Override
                        public void onConfigured(CameraCaptureSession session) {
                            mSession = session;
                            try {
                                CaptureRequest.Builder b =
                                        mDevice.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                                b.addTarget(mPreviewReader.getSurface());
                                session.setRepeatingRequest(b.build(), null, mHandler);
                                log("preview running");
                            } catch (CameraAccessException | RuntimeException e) {
                                done(RESULT_CANCELED, null, "preview: " + e);
                            }
                        }

                        @Override
                        public void onConfigureFailed(CameraCaptureSession session) {
                            done(RESULT_CANCELED, null, "the capture session failed");
                        }
                    }));
        } catch (CameraAccessException | RuntimeException e) {
            done(RESULT_CANCELED, null, "session: " + e);
        }
    }

    /** A preview frame: shown a few times a second (YUV converted to a Bitmap here). */
    private void showPreview(ImageReader reader) {
        Image image = reader.acquireLatestImage();
        if (image == null) return;
        try {
            long now = System.currentTimeMillis();
            if (now - mLastShown < 200) return;
            mLastShown = now;
            Bitmap b = bitmap(image);
            mMain.post(() -> mPreview.setImageBitmap(b));
        } catch (Throwable t) {
            // a frame not shown
        } finally {
            image.close();
        }
    }

    /** A YUV_420_888 image as an ARGB Bitmap (BT.601). */
    private static Bitmap bitmap(Image image) {
        int w = image.getWidth();
        int h = image.getHeight();
        Image.Plane[] p = image.getPlanes();
        ByteBuffer y = p[0].getBuffer();
        ByteBuffer u = p[1].getBuffer();
        ByteBuffer v = p[2].getBuffer();
        int yRow = p[0].getRowStride();
        int yPixel = p[0].getPixelStride();
        int cRow = p[1].getRowStride();
        int cPixel = p[1].getPixelStride();
        int[] argb = new int[w * h];
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                int c = (y.get(j * yRow + i * yPixel) & 0xff) - 16;
                int at = (j / 2) * cRow + (i / 2) * cPixel;
                int d = (u.get(at) & 0xff) - 128;
                int e = (v.get(at) & 0xff) - 128;
                int r = clamp((298 * c + 409 * e + 128) >> 8);
                int g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
                int bl = clamp((298 * c + 516 * d + 128) >> 8);
                argb[j * w + i] = 0xff000000 | (r << 16) | (g << 8) | bl;
            }
        }
        return Bitmap.createBitmap(argb, w, h, Bitmap.Config.ARGB_8888);
    }

    private static int clamp(int v) {
        return v < 0 ? 0 : v > 255 ? 255 : v;
    }
    private void capture() {
        mHandler.post(() -> {
            if (mSession == null || mDone) {
                if (!mDone) mMain.postDelayed(this::capture, 300);  // not open yet
                return;
            }
            try {
                CaptureRequest.Builder b =
                        mDevice.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
                b.addTarget(mStillReader.getSurface());
                b.set(CaptureRequest.JPEG_QUALITY, (byte) 90);
                mSession.capture(b.build(), null, mHandler);
                log("taking a photo");
            } catch (CameraAccessException | RuntimeException e) {
                done(RESULT_CANCELED, null, "capture: " + e);
            }
        });
    }

    private void stillTaken(ImageReader reader) {
        Image image = reader.acquireNextImage();
        if (image == null) return;
        byte[] jpeg;
        int w = image.getWidth();
        int h = image.getHeight();
        try {
            ByteBuffer b = image.getPlanes()[0].getBuffer();
            jpeg = new byte[b.remaining()];
            b.get(jpeg);
        } finally {
            image.close();
        }
        Uri output = getIntent().getParcelableExtra(MediaStore.EXTRA_OUTPUT, Uri.class);
        if (output != null) {
            try (OutputStream out = getContentResolver().openOutputStream(output)) {
                out.write(jpeg);
            } catch (Exception e) {
                done(RESULT_CANCELED, null, "writing " + output + ": " + e);
                return;
            }
            done(RESULT_OK, new Intent().setData(output),
                    "a " + w + "x" + h + " JPEG (" + jpeg.length + " bytes) written to " + output);
            return;
        }
        Bitmap full = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
        if (full == null) {
            done(RESULT_CANCELED, null, "the photo does not decode");
            return;
        }
        int tw = THUMBNAIL_WIDTH;
        int th = Math.max(1, full.getHeight() * tw / full.getWidth());
        Bitmap thumbnail = Bitmap.createScaledBitmap(full, tw, th, true);
        done(RESULT_OK, new Intent("inline-data").putExtra("data", thumbnail),
                "a " + w + "x" + h + " photo, returned as a " + tw + "x" + th + " thumbnail");
    }

    private void done(int result, Intent data, String why) {
        mMain.post(() -> {
            if (mDone) return;
            mDone = true;
            log((result == RESULT_OK ? "done: " : "cancelled: ") + why);
            setResult(result, data);
            finish();
        });
    }

    @Override
    protected void onDestroy() {
        mDone = true;
        mHandler.post(() -> {
            if (mSession != null) mSession.close();
            if (mDevice != null) mDevice.close();
            if (mPreviewReader != null) mPreviewReader.close();
            if (mStillReader != null) mStillReader.close();
            mThread.quitSafely();
        });
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        done(RESULT_CANCELED, null, "back");
    }
}
