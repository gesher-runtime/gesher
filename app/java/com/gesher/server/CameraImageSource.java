/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.RectF;
import android.media.ExifInterface;
import android.util.Size;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The Control Center's camera image-picker setting ({@link CameraImageFallback}) on the
 * Camera and Camera2 side, in the app's process (docs/devices.md, "Choosing an image instead
 * of the camera"). While the setting is on:
 *
 * <ul>
 * <li>the camera list ends with {@link #DEVICE_ID}, a rear camera whose frames are an image
 *     the user selects with the Windows file picker when an app opens it;
 * <li>opening a PC camera asks the user once whether to use the camera or a selected image.
 * </ul>
 *
 * <p>The question and the file picker are the system server's ({@link CameraSourceService}):
 * the app's process neither reads the setting nor opens the PC's files itself, it receives
 * the selected image as a read-only descriptor. A client (one app's open of a camera, until
 * it closes it) is asked once: a new size, a picture or a restarted preview keep its answer,
 * and a cancelled question fails its opens rather than asking again.
 */
public final class CameraImageSource {
    /** The selected-image camera's id while the setting is on. */
    public static final String DEVICE_ID = "gesher-image-picker";
    static final int ERROR_CANCELLED = 0x800704C7;  // HRESULT_FROM_WIN32(ERROR_CANCELLED)
    /** The largest selected image read (bytes); a larger file is refused. */
    static final int MAX_IMAGE_BYTES = 32 * 1024 * 1024;

    /** What {@link CameraSourceService} answers; host tests replace it. */
    interface Host {
        /** Whether the setting is on (false when the service cannot be reached). */
        boolean enabled();

        /**
         * Asks the user for {@code cameraId}'s source: for a PC camera ({@code physical})
         * the camera or a selected image, for {@link #DEVICE_ID} an image.
         */
        Selection choose(String cameraId, boolean physical);
    }

    /** The user's answer: the camera, a selected image (its file's bytes) or a cancel. */
    static final class Selection {
        static final int CAMERA = 0, IMAGE = 1, CANCELLED = 2;
        static final Selection USE_CAMERA = new Selection(CAMERA, null);
        static final Selection NONE = new Selection(CANCELLED, null);

        final int kind;
        final byte[] image;

        Selection(int kind, byte[] image) {
            if ((kind == IMAGE) != (image != null)) throw new IllegalArgumentException("selection");
            this.kind = kind;
            this.image = image;
        }
    }

    static volatile Host sHost = CameraSourceService.CLIENT;

    private CameraImageSource() {
    }

    /** Whether the setting is on, as the system server reads it. */
    static boolean enabled() {
        return sHost.enabled();
    }

    /** {@code physical} (the PC's cameras) and, when {@code enabled}, {@link #DEVICE_ID}. */
    static List<WindowsCamera.Device> withPicker(List<WindowsCamera.Device> physical,
            boolean enabled) {
        if (!enabled) return physical;
        List<WindowsCamera.Device> all = new ArrayList<>(physical.size() + 1);
        all.addAll(physical);
        // The sizes a selected image is offered at (4:3 and 16:9, largest first): an image
        // is scaled into the configured size, letterboxed when its shape differs.
        all.add(new WindowsCamera.Device(DEVICE_ID, "Gesher Image Picker", List.of(
                new Size(2592, 1944), new Size(1920, 1080), new Size(1280, 960),
                new Size(1280, 720), new Size(640, 480)), true, false));
        return Collections.unmodifiableList(all);
    }

    /** One client's answer, kept from its first open until it closes the camera. */
    public static final class Choice {
        private Selection mSelection;
        /** The selected image's last frame and its size (a picture or reopen reuses it). */
        private byte[] mFrame;
        private int mFrameWidth, mFrameHeight;

        synchronized Selection get() {
            return mSelection;
        }

        synchronized void set(Selection selection) {
            if (mSelection == null) mSelection = selection;
        }

        /** Whether the client's frames are a selected image (fixed focus, whatever the device). */
        synchronized boolean image() {
            return mSelection != null && mSelection.kind == Selection.IMAGE;
        }

        /**
         * Forgets a cancelled answer, so the app's next explicit start asks again (the
         * legacy startPreview: an app retries it; a Camera2 frame loop never resets).
         */
        synchronized void resetCancelled() {
            if (mSelection != null && mSelection.kind == Selection.CANCELLED) mSelection = null;
        }

        synchronized byte[] frame(int width, int height) {
            return mFrame != null && mFrameWidth == width && mFrameHeight == height ? mFrame : null;
        }

        synchronized void keepFrame(byte[] frame, int width, int height) {
            mFrame = frame;
            mFrameWidth = width;
            mFrameHeight = height;
        }
    }

    /**
     * Asks {@code choice}'s client for {@code id}'s source if it has not answered: call it
     * outside the client's locks, as the question waits for the user.
     */
    static Selection decide(String id, Choice choice) {
        Selection selection = choice.get();
        if (selection != null) return selection;
        boolean picker = DEVICE_ID.equals(id);
        if (!picker && !enabled()) {
            selection = Selection.USE_CAMERA;
        } else {
            selection = sHost.choose(id, !picker);
            if (selection == null || (picker && selection.kind == Selection.CAMERA)) {
                selection = Selection.NONE;
            }
        }
        choice.set(selection);
        return choice.get();
    }

    /**
     * Opens {@code id} for {@code choice}'s client at the size nearest to {@code width} x
     * {@code height}: the PC camera, or the selected image scaled to that size.
     */
    static WindowsCamera.Capture open(String id, int width, int height, Choice choice)
            throws WindowsCamera.OpenException {
        Selection selection = decide(id, choice);
        switch (selection.kind) {
            case Selection.CAMERA:
                return WindowsCamera.open(id, width, height);
            case Selection.IMAGE: {
                int w = Math.max(2, width & ~1);
                int h = Math.max(2, height & ~1);
                try {
                    byte[] frame = choice.frame(w, h);
                    if (frame == null) {
                        frame = toNv12(selection.image, w, h);
                        choice.keepFrame(frame, w, h);
                    }
                    return WindowsCamera.Capture.ofFrame(frame, w, h);
                } catch (IOException | RuntimeException | OutOfMemoryError e) {
                    CameraService.log("camera " + id + ": the selected image: " + e);
                    throw new WindowsCamera.OpenException(id, 0x80004005);  // E_FAIL
                }
            }
            default:
                throw new WindowsCamera.OpenException(id, ERROR_CANCELLED);
        }
    }

    /**
     * {@code encoded} (a JPEG, PNG, BMP, WebP or GIF file's bytes), scaled with its aspect
     * kept and centred on black, as one NV12 frame of {@code width} x {@code height} (even).
     */
    static byte[] toNv12(byte[] encoded, int width, int height) throws IOException {
        if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
            throw new IllegalArgumentException("NV12 dimensions must be positive and even: "
                    + width + "x" + height);
        }
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(encoded, 0, encoded.length, bounds);
        BitmapFactory.Options options = new BitmapFactory.Options();
        int sample = 1;
        while (bounds.outWidth / (sample * 2) >= width && bounds.outHeight / (sample * 2) >= height) {
            sample *= 2;
        }
        options.inSampleSize = sample;
        Bitmap decoded = BitmapFactory.decodeByteArray(encoded, 0, encoded.length, options);
        if (decoded == null) throw new IOException("not an image Android decodes");
        // A phone's photo is often stored sideways with an EXIF orientation, which Windows
        // applies when it shows the file: so does the camera's frame.
        Bitmap source = decoded;
        Matrix orientation = exifOrientation(encoded);
        if (orientation != null) {
            source = Bitmap.createBitmap(decoded, 0, 0, decoded.getWidth(), decoded.getHeight(),
                    orientation, true);
            if (source != decoded) decoded.recycle();
        }
        Bitmap frame = null;
        try {
            frame = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(frame);
            canvas.drawColor(Color.BLACK);
            int sw = source.getWidth();
            int sh = source.getHeight();
            if (sw > 0 && sh > 0) {
                float scale = Math.min((float) width / sw, (float) height / sh);
                float dw = sw * scale;
                float dh = sh * scale;
                float left = (width - dw) / 2;
                float top = (height - dh) / 2;
                canvas.drawBitmap(source, null, new RectF(left, top, left + dw, top + dh),
                        new Paint(Paint.FILTER_BITMAP_FLAG));
            }
            int[] argb = new int[width * height];
            frame.getPixels(argb, 0, width, 0, 0, width, height);
            return argbToNv12(argb, width, height);
        } finally {
            source.recycle();
            if (frame != null) frame.recycle();
        }
    }

    /** The EXIF orientation's transform, or null when the image is stored upright. */
    static Matrix exifOrientation(byte[] encoded) {
        int tag;
        try {
            tag = new ExifInterface(new ByteArrayInputStream(encoded)).getAttributeInt(
                    ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
        } catch (IOException | RuntimeException e) {
            return null;  // no EXIF (PNG, BMP...) or none readable: as stored
        }
        Matrix m = new Matrix();
        switch (tag) {
            case ExifInterface.ORIENTATION_FLIP_HORIZONTAL: m.setScale(-1, 1); break;
            case ExifInterface.ORIENTATION_ROTATE_180: m.setRotate(180); break;
            case ExifInterface.ORIENTATION_FLIP_VERTICAL: m.setScale(1, -1); break;
            case ExifInterface.ORIENTATION_TRANSPOSE: m.setRotate(90); m.postScale(-1, 1); break;
            case ExifInterface.ORIENTATION_ROTATE_90: m.setRotate(90); break;
            case ExifInterface.ORIENTATION_TRANSVERSE: m.setRotate(-90); m.postScale(-1, 1); break;
            case ExifInterface.ORIENTATION_ROTATE_270: m.setRotate(-90); break;
            default: return null;
        }
        return m;
    }

    /**
     * BT.601 limited range, as the cameras' NV12 (CameraFrames converts it back); each
     * chroma sample is its 2x2 block's average.
     */
    static byte[] argbToNv12(int[] argb, int width, int height) {
        int ySize = width * height;
        byte[] nv12 = new byte[ySize * 3 / 2];
        for (int row = 0; row < height; row++) {
            int rowOffset = row * width;
            for (int col = 0; col < width; col++) {
                int px = argb[rowOffset + col];
                int r = (px >> 16) & 0xFF;
                int g = (px >> 8) & 0xFF;
                int b = px & 0xFF;
                nv12[rowOffset + col] = (byte) clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            }
        }
        for (int row = 0; row < height; row += 2) {
            int uvRowOffset = ySize + (row >> 1) * width;
            for (int col = 0; col < width; col += 2) {
                int r = 0, g = 0, b = 0;
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        int px = argb[(row + dy) * width + col + dx];
                        r += (px >> 16) & 0xFF;
                        g += (px >> 8) & 0xFF;
                        b += px & 0xFF;
                    }
                }
                r = (r + 2) >> 2;
                g = (g + 2) >> 2;
                b = (b + 2) >> 2;
                nv12[uvRowOffset + col] = (byte) clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                nv12[uvRowOffset + col + 1] =
                        (byte) clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
        return nv12;
    }

    private static int clamp(int value) {
        return value < 0 ? 0 : value > 255 ? 255 : value;
    }
}
