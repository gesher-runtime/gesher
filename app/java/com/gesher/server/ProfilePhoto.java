/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.ByteArrayOutputStream;
import java.io.IOException;

/** Derives Profile photos from an actual decoded image. */
record ProfilePhoto(byte[] thumbnail, byte[] display) {
    static final int THUMBNAIL_DIMENSION = 96;
    static final int DISPLAY_DIMENSION = 720;
    static final int MAX_INPUT_BYTES = 16 * 1024 * 1024;

    static ProfilePhoto decode(byte[] encoded) throws IOException {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(encoded, 0, encoded.length, bounds);
        int extent = Math.max(bounds.outWidth, bounds.outHeight);
        if (extent <= 0) throw new IOException("Invalid Contacts Profile photo");
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inSampleSize = Math.max(1, Integer.highestOneBit(extent / DISPLAY_DIMENSION));
        Bitmap bitmap = BitmapFactory.decodeByteArray(encoded, 0, encoded.length, options);
        if (bitmap == null) throw new IOException("Could not decode Contacts Profile photo");
        try {
            byte[] thumbnail = scaledJpeg(bitmap, THUMBNAIL_DIMENSION);
            byte[] display = extent > THUMBNAIL_DIMENSION ? scaledJpeg(bitmap, DISPLAY_DIMENSION) : null;
            return new ProfilePhoto(thumbnail, display);
        } finally {
            bitmap.recycle();
        }
    }

    private static byte[] scaledJpeg(Bitmap original, int maximum) throws IOException {
        int width = original.getWidth();
        int height = original.getHeight();
        int extent = Math.max(width, height);
        Bitmap scaled = extent <= maximum ? original : Bitmap.createScaledBitmap(original,
                Math.max(1, (int) ((long) width * maximum / extent)),
                Math.max(1, (int) ((long) height * maximum / extent)), true);
        try {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            if (!scaled.compress(Bitmap.CompressFormat.JPEG, 92, bytes)) {
                throw new IOException("Could not encode Contacts Profile photo");
            }
            return bytes.toByteArray();
        } finally {
            if (scaled != original) scaled.recycle();
        }
    }
}
