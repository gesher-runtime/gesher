/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.surfaceapp;

import android.graphics.Bitmap;
import android.graphics.ColorSpace;

/** Public Android API checks; input pixels are explicit test application data. */
final class BitmapSharedContract {
    static final String DESCRIPTOR = "com.gesher.surfaceapp.BitmapRelay";
    static final int STORE = 1, FORWARD = 2, EMPTY = 3;
    static void require(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }
    static int pixel(int x, int y) {
        return 0xff000000 | ((x * 73 + y * 19) & 255) << 16
                | ((x * 11 + y * 97) & 255) << 8 | ((x * 53 + y * 7) & 255);
    }
    static Bitmap create(int width, int height) {
        Bitmap mutable = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        int[] pixels = new int[width * height];
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            pixels[y * width + x] = pixel(x, y);
        mutable.setPixels(pixels, 0, width, 0, 0, width, height);
        mutable.setDensity(231);
        Bitmap shared = null;
        try {
            shared = mutable.asShared();
            require(shared != mutable, "mutable input must produce an independent snapshot");
            mutable.eraseColor(0xffcc2255);
            verify(shared);
            return shared;
        } catch (Throwable failure) {
            if (shared != null && shared != mutable) shared.recycle();
            throw failure;
        } finally { mutable.recycle(); }
    }
    static void verify(Bitmap bitmap) {
        require(bitmap != null && !bitmap.isRecycled() && !bitmap.isMutable(), "live immutable Bitmap");
        Bitmap same = bitmap.asShared();
        if (same != bitmap) { same.recycle(); throw new IllegalStateException("Bitmap must retain actual shared storage"); }
        require(bitmap.getConfig() == Bitmap.Config.ARGB_8888 && bitmap.getDensity() == 231,
                "config and density survive shared storage");
        require(bitmap.getColorSpace().equals(ColorSpace.get(ColorSpace.Named.SRGB)), "color space survives Parcel");
        int width = bitmap.getWidth(), height = bitmap.getHeight();
        int[] pixels = new int[width * height];
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            if (pixels[y * width + x] != pixel(x, y))
                throw new IllegalStateException("pixel " + x + "," + y);
    }
}
