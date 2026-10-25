/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.graphics.Bitmap;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.YuvImage;
import android.view.Surface;

import java.io.ByteArrayOutputStream;

/**
 * Frames from a camera into the surfaces of a capture request (compat/devices/
 * camera_frames.cc): an ImageReader's (Gesher's own natives: YUV_420_888, RGBA_8888, JPEG,
 * PRIVATE), or a surface with a native window (drawn as RGBA).
 */
public final class CameraFrames {
    private CameraFrames() {
    }

    /** Delivered. */
    public static final int OK = 0;
    /** The surface cannot take frames (no native window, or a JPEG reader and no JPEG). */
    public static final int UNSUPPORTED = 1;

    /** Hands {@code nv12} (w x h) to {@code surface}; {@code jpeg} for a JPEG reader. */
    public static int deliver(Surface surface, byte[] nv12, int w, int h, long timestampNs,
            byte[] jpeg) {
        return nativeDeliver(surface, nv12, w, h, timestampNs, jpeg);
    }

    /** Whether {@code surface} is an ImageReader's. */
    public static boolean isReader(Surface surface) {
        return nativeIsReader(surface);
    }

    /**
     * {@code nv12} (w x h) as a JPEG of tw x th: YuvImage's encoder (hwui's), else a
     * Bitmap's.
     */
    public static byte[] jpeg(byte[] nv12, int w, int h, int tw, int th, int quality) {
        byte[] nv21 = nativeToNv21(nv12, w, h, tw, th);
        ByteArrayOutputStream out = new ByteArrayOutputStream(tw * th / 4);
        try {
            new YuvImage(nv21, ImageFormat.NV21, tw, th, null)
                    .compressToJpeg(new Rect(0, 0, tw, th), quality, out);
            return out.toByteArray();
        } catch (Throwable yuvImageFailed) {
            out.reset();
            int[] argb = new int[tw * th];
            int ySize = tw * th;
            for (int y = 0; y < th; y++) {
                for (int x = 0; x < tw; x++) {
                    int c = (nv21[y * tw + x] & 0xff) - 16;
                    int uvAt = ySize + (y / 2) * tw + (x / 2) * 2;
                    int e = (nv21[uvAt] & 0xff) - 128;      // V
                    int d = (nv21[uvAt + 1] & 0xff) - 128;  // U
                    int r = clamp((298 * c + 409 * e + 128) >> 8);
                    int g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
                    int b = clamp((298 * c + 516 * d + 128) >> 8);
                    argb[y * tw + x] = 0xff000000 | (r << 16) | (g << 8) | b;
                }
            }
            Bitmap bitmap = Bitmap.createBitmap(argb, tw, th, Bitmap.Config.ARGB_8888);
            bitmap.compress(Bitmap.CompressFormat.JPEG, quality, out);
            bitmap.recycle();
            return out.toByteArray();
        }
    }

    /** {@code nv12} (w x h) as NV21 of tw x th (the legacy Camera's preview format). */
    public static byte[] nv21(byte[] nv12, int w, int h, int tw, int th) {
        return nativeToNv21(nv12, w, h, tw, th);
    }

    private static int clamp(int v) {
        return v < 0 ? 0 : v > 255 ? 255 : v;
    }

    private static native int nativeDeliver(Surface surface, byte[] nv12, int w, int h,
            long timestampNs, byte[] jpeg);

    private static native boolean nativeIsReader(Surface surface);

    private static native byte[] nativeToNv21(byte[] nv12, int w, int h, int tw, int th);
}
