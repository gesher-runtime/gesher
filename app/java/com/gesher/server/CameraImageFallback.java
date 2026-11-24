/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.util.Properties;

/**
 * Control Center setting and shared image-conversion helpers for choosing a PC image
 * instead of live camera capture or streaming a chosen image through Camera/Camera2.
 *
 * <p>Stored in {@code <Device.dataDir()>/system/camera.properties} under
 * {@code image_picker_fallback=true} (off by default).
 */
public final class CameraImageFallback {
    private static final String KEY = "image_picker_fallback";

    /** Package-private hook so unit tests can override without touching disk. */
    static volatile Boolean sTestOverride;

    private CameraImageFallback() {
    }

    /** Path to {@code <Device.dataDir()>/system/camera.properties}. */
    public static File file() {
        return new File(Device.dataDir(), "system/camera.properties");
    }

    /**
     * Whether the image-picker fallback is enabled. Reads {@link #file()} dynamically on
     * each call so Control Center changes take effect immediately without a restart.
     * Returns {@code false} if the file is missing, unreadable, or {@code Device.dataDir()}
     * is unavailable.
     */
    public static boolean enabled() {
        Boolean override = sTestOverride;
        if (override != null) return override;
        try {
            String dataDir = Device.dataDir();
            if (dataDir == null || dataDir.isEmpty()) return false;
            File propsFile = new File(dataDir, "system/camera.properties");
            if (!propsFile.isFile() || !propsFile.canRead()) return false;
            Properties props = new Properties();
            try (FileInputStream in = new FileInputStream(propsFile)) {
                props.load(in);
            }
            return "true".equalsIgnoreCase(props.getProperty(KEY, "").trim());
        } catch (IOException | RuntimeException | LinkageError e) {
            return false;
        }
    }

    /** Updates or removes {@code image_picker_fallback} in {@link #file()}. */
    public static synchronized void save(boolean enabled) throws IOException {
        File propsFile = file();
        Properties props = new Properties();
        if (propsFile.isFile()) {
            try (FileInputStream in = new FileInputStream(propsFile)) {
                props.load(in);
            }
        }
        if (enabled) {
            props.setProperty(KEY, "true");
        } else {
            props.remove(KEY);
        }
        File parent = propsFile.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs() && !parent.isDirectory()) {
            throw new IOException("Could not create directory: " + parent);
        }
        try (FileOutputStream out = new FileOutputStream(propsFile)) {
            props.store(out, null);
        }
    }

    /**
     * Encodes {@code imageFile} as JPEG bytes. If the file already starts with the JPEG
     * SOI marker ({@code 0xFF 0xD8}), returns its bytes directly; otherwise decodes and
     * re-encodes at quality 95, falling back to raw bytes if {@link BitmapFactory} is
     * unavailable in host tests.
     */
    static byte[] encodeJpeg(File imageFile) throws IOException {
        byte[] raw = Files.readAllBytes(imageFile.toPath());
        if (raw.length >= 2 && (raw[0] & 0xFF) == 0xFF && (raw[1] & 0xFF) == 0xD8) {
            return raw;
        }
        try {
            Bitmap bitmap = BitmapFactory.decodeFile(imageFile.getPath());
            if (bitmap == null) {
                throw new IOException("Could not decode image: " + imageFile);
            }
            try {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 95, out)) {
                    throw new IOException("Could not encode JPEG: " + imageFile);
                }
                return out.toByteArray();
            } finally {
                bitmap.recycle();
            }
        } catch (LinkageError unavailable) {
            return raw;
        }
    }

    /**
     * Decodes {@code imageFile} as a thumbnail scaled proportionally to {@code targetWidth}.
     * Returns {@code null} if decoding fails or {@link BitmapFactory} is unavailable.
     */
    static Bitmap decodeThumbnail(File imageFile, int targetWidth) {
        if (targetWidth <= 0) return null;
        try {
            Bitmap bitmap = BitmapFactory.decodeFile(imageFile.getPath());
            if (bitmap == null) return null;
            int w = bitmap.getWidth();
            int h = bitmap.getHeight();
            if (w <= 0 || h <= 0) return bitmap;
            int targetHeight = Math.max(1, Math.round((float) h * targetWidth / w));
            Bitmap scaled = Bitmap.createScaledBitmap(bitmap, targetWidth, targetHeight, true);
            if (scaled != bitmap) bitmap.recycle();
            return scaled;
        } catch (LinkageError unavailable) {
            return null;
        }
    }

    /**
     * Decodes {@code imageFile}, scales it preserving aspect ratio (centered on a black
     * background) to {@code width} x {@code height}, and converts the ARGB pixels to NV12
     * ({@code width * height * 3 / 2} bytes: Y plane followed by interleaved UV plane).
     */
    static byte[] toNv12(File imageFile, int width, int height) throws IOException {
        if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
            throw new IllegalArgumentException("NV12 dimensions must be positive even integers: "
                    + width + "x" + height);
        }
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(imageFile.getPath(), bounds);
        BitmapFactory.Options opts = new BitmapFactory.Options();
        if (bounds.outWidth > width * 2 && bounds.outHeight > height * 2) {
            int sample = 1;
            while ((bounds.outWidth / (sample * 2)) >= width
                    && (bounds.outHeight / (sample * 2)) >= height) {
                sample *= 2;
            }
            opts.inSampleSize = sample;
        }
        Bitmap src = BitmapFactory.decodeFile(imageFile.getPath(), opts);
        if (src == null) {
            throw new IOException("Could not decode image: " + imageFile);
        }
        Bitmap frame = null;
        try {
            frame = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(frame);
            canvas.drawColor(Color.BLACK);
            int sw = src.getWidth();
            int sh = src.getHeight();
            if (sw > 0 && sh > 0) {
                float scale = Math.min((float) width / sw, (float) height / sh);
                float dw = sw * scale;
                float dh = sh * scale;
                float left = (width - dw) * 0.5f;
                float top = (height - dh) * 0.5f;
                Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
                canvas.drawBitmap(src, null, new RectF(left, top, left + dw, top + dh), paint);
            }
            int[] argb = new int[width * height];
            frame.getPixels(argb, 0, width, 0, 0, width, height);
            return argbToNv12(argb, width, height);
        } finally {
            src.recycle();
            if (frame != null) frame.recycle();
        }
    }

    private static byte[] argbToNv12(int[] argb, int width, int height) {
        int ySize = width * height;
        byte[] nv12 = new byte[ySize * 3 / 2];
        for (int row = 0; row < height; row++) {
            int rowOffset = row * width;
            int uvRowOffset = ySize + (row >> 1) * width;
            for (int col = 0; col < width; col++) {
                int px = argb[rowOffset + col];
                int r = (px >> 16) & 0xFF;
                int g = (px >> 8) & 0xFF;
                int b = px & 0xFF;
                int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                nv12[rowOffset + col] = (byte) clampByte(y);
                if ((row & 1) == 0 && (col & 1) == 0) {
                    int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                    nv12[uvRowOffset + col] = (byte) clampByte(u);
                    nv12[uvRowOffset + col + 1] = (byte) clampByte(v);
                }
            }
        }
        return nv12;
    }

    private static int clampByte(int value) {
        return value < 0 ? 0 : (value > 255 ? 255 : value);
    }
}
