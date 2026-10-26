/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.surfaceapp;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.graphics.YuvImage;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Parcel;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Checks that need no window: Bitmap parcels, YuvImage, a SurfaceTexture's producer. */
final class Checks {
    private Checks() {
    }

    static void run(SurfaceActivity a) {
        try {
            parcel(16, 8);       // 512 bytes: in place
            parcel(400, 300);    // 480000 bytes: shared memory where the parcel takes fds
        } catch (Throwable t) {
            SurfaceActivity.say("bitmap parcel failed: " + t);
        }
        try {
            yuv();
        } catch (Throwable t) {
            SurfaceActivity.say("YuvImage failed: " + t);
        }
        try {
            surfaceTexture();
        } catch (Throwable t) {
            SurfaceActivity.say("SurfaceTexture failed: " + t);
        }
    }

    /** A Bitmap written to a Parcel and read back: size, config, mutability and pixels. */
    static void parcel(int w, int h) {
        Bitmap b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                b.setPixel(x, y, Color.rgb(x * 255 / w, y * 255 / h, 128));
            }
        }
        Parcel p = Parcel.obtain();
        b.writeToParcel(p, 0);
        int size = p.dataSize();
        p.setDataPosition(0);
        Bitmap r = Bitmap.CREATOR.createFromParcel(p);
        p.recycle();
        int wrong = 0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (r.getPixel(x, y) != b.getPixel(x, y)) wrong++;
            }
        }
        SurfaceActivity.say("bitmap parcel " + w + "x" + h + ": parcel " + (size > w * h * 4
                ? "holds the pixels" : "holds a descriptor") + ", read back " + r.getWidth() + "x"
                + r.getHeight() + " " + r.getConfig() + " mutable " + r.isMutable() + ", "
                + wrong + " pixels differ");
    }

    /** An NV21 image of two colors (left gray-blue, right orange) to JPEG and back. */
    static void yuv() {
        int w = 64, h = 32;
        byte[] nv21 = new byte[w * h * 3 / 2];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                nv21[y * w + x] = (byte) (x < w / 2 ? 90 : 170);
            }
        }
        for (int y = 0; y < h / 2; y++) {
            for (int x = 0; x < w / 2; x++) {
                int i = w * h + y * w + x * 2;
                boolean left = x < w / 4;
                nv21[i] = (byte) (left ? 110 : 190);      // V (Cr)
                nv21[i + 1] = (byte) (left ? 170 : 60);   // U (Cb)
            }
        }
        YuvImage img = new YuvImage(nv21, ImageFormat.NV21, w, h, null);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        boolean ok = img.compressToJpeg(new Rect(0, 0, w, h), 90, out);
        byte[] jpeg = out.toByteArray();
        Bitmap d = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
        String colors = d == null ? "does not decode" : "decodes to " + d.getWidth() + "x"
                + d.getHeight() + ", left " + rgb(d.getPixel(8, 16)) + ", right "
                + rgb(d.getPixel(56, 16));
        SurfaceActivity.say("YuvImage NV21 compressToJpeg " + ok + ", " + jpeg.length
                + " bytes, starts FFD8 " + (jpeg.length > 2 && (jpeg[0] & 0xff) == 0xff
                        && (jpeg[1] & 0xff) == 0xd8) + ", " + colors);
    }

    static String rgb(int c) {
        return Color.red(c) + "," + Color.green(c) + "," + Color.blue(c);
    }

    /** A SurfaceTexture's producer: a Surface over it, a Canvas frame, the frame listener. */
    static void surfaceTexture() throws InterruptedException {
        HandlerThread t = new HandlerThread("st");
        t.start();
        SurfaceTexture st = new SurfaceTexture(false);
        st.setDefaultBufferSize(64, 32);
        CountDownLatch frame = new CountDownLatch(1);
        st.setOnFrameAvailableListener(s -> frame.countDown(), new Handler(t.getLooper()));
        Surface s = new Surface(st);
        Canvas c = s.lockCanvas(null);
        String size = c.getWidth() + "x" + c.getHeight();
        c.drawColor(Color.RED);
        s.unlockCanvasAndPost(c);
        boolean got = frame.await(5, TimeUnit.SECONDS);
        float[] m = new float[16];
        st.getTransformMatrix(m);
        SurfaceActivity.say("SurfaceTexture: Surface.lockCanvas " + size
                + ", onFrameAvailable " + got + ", timestamp " + st.getTimestamp()
                + ", transform [" + m[0] + "," + m[5] + "," + m[13] + "]");
        s.release();
        st.release();
        SurfaceActivity.say("SurfaceTexture: released " + st.isReleased());
        t.quitSafely();
    }
}
