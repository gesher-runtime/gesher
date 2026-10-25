/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devices;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.hardware.Camera;
import android.os.Handler;
import android.os.HandlerThread;

import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * The legacy android.hardware.Camera over GESHER_CAMERA_TEST's camera (hwsvc2, ADR 0051):
 * info, parameters, a preview at 320x240 into callback buffers (NV21), autofocus, a
 * picture (shutter, raw, JPEG), release. The camera is opened on a thread with a Looper
 * of its own, where its callbacks come (Camera's EventHandler).
 */
@SuppressWarnings("deprecation")
final class LegacyCameraSteps {
    private final HandlerThread mThread = new HandlerThread("legacy-camera");
    private final BlockingQueue<Object> mEvents = new LinkedBlockingQueue<>();

    private String next(long timeoutMs) throws InterruptedException {
        Object e = mEvents.poll(timeoutMs, TimeUnit.MILLISECONDS);
        return e == null ? "(nothing within " + timeoutMs + " ms)" : e.toString();
    }

    private static String sizes(List<Camera.Size> sizes) {
        StringBuilder b = new StringBuilder("[");
        for (Camera.Size s : sizes) {
            if (b.length() > 1) b.append(", ");
            b.append(s.width).append('x').append(s.height);
        }
        return b.append(']').toString();
    }

    void run() throws Exception {
        int n = Camera.getNumberOfCameras();
        int id = n - 1;  // the test camera comes last
        Camera.CameraInfo info = new Camera.CameraInfo();
        Camera.getCameraInfo(id, info);
        Report.line("camera: legacy " + n + " cameras, " + id + " facing " + info.facing
                + ", orientation " + info.orientation);
        mThread.start();
        Handler h = new Handler(mThread.getLooper());
        Camera[] camera = new Camera[1];
        h.post(() -> {
            try {
                camera[0] = Camera.open(id);
                mEvents.add("opened");
            } catch (RuntimeException e) {
                mEvents.add("open failed: " + e);
            }
        });
        Report.line("camera: legacy open(" + id + ") " + next(10000));
        Camera c = camera[0];
        if (c == null) {
            mThread.quitSafely();
            return;
        }
        Camera.Parameters p = c.getParameters();
        Report.line("camera: legacy parameters: preview sizes " + sizes(p.getSupportedPreviewSizes())
                + ", preview format " + p.getPreviewFormat() + ", picture sizes "
                + sizes(p.getSupportedPictureSizes()) + ", focus " + p.getFocusMode()
                + ", zoom " + p.isZoomSupported() + ", flash " + p.getSupportedFlashModes());
        p.setPreviewSize(320, 240);
        p.setPictureSize(320, 240);
        c.setParameters(p);
        int frameSize = 320 * 240 * 3 / 2;
        int[] frames = new int[1];
        c.setPreviewCallbackWithBuffer((data, cam) -> {
            if (++frames[0] == 1) {
                mEvents.add("frame " + data.length + " bytes, Y(10,100)=" + (data[100 * 320 + 10] & 0xff));
            }
            cam.addCallbackBuffer(data);
        });
        for (int i = 0; i < 3; i++) c.addCallbackBuffer(new byte[frameSize]);
        c.startPreview();
        Report.line("camera: legacy preview at " + c.getParameters().getPreviewSize().width + "x"
                + c.getParameters().getPreviewSize().height + ": first " + next(10000)
                + ", previewEnabled " + c.previewEnabled());
        Thread.sleep(1000);
        Report.line("camera: legacy preview frames keep coming: " + (frames[0] >= 5));
        c.autoFocus((success, cam) -> mEvents.add("autoFocus " + success));
        Report.line("camera: legacy " + next(5000));
        c.takePicture(() -> mEvents.add("shutter"),
                (data, cam) -> mEvents.add("raw " + (data == null ? "null" : data.length + " bytes")),
                (data, cam) -> {
                    Bitmap b = data != null ? BitmapFactory.decodeByteArray(data, 0, data.length) : null;
                    mEvents.add("JPEG starts FFD8 " + (data != null && data.length > 2
                            && (data[0] & 0xff) == 0xff && (data[1] & 0xff) == 0xd8)
                            + ", decodes to " + (b == null ? "nothing" : b.getWidth() + "x" + b.getHeight()));
                });
        String shutter = next(10000);
        String raw = next(5000);
        String jpeg = next(5000);
        Report.line("camera: legacy takePicture: " + shutter + ", " + raw + ", " + jpeg
                + "; previewEnabled after " + c.previewEnabled());
        c.release();
        Report.line("camera: legacy released");
        mThread.quitSafely();
    }
}
