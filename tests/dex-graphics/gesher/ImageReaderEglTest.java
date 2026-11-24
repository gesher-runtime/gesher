/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher;

import android.graphics.PixelFormat;
import android.graphics.ImageFormat;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Bitmap;
import android.graphics.ColorSpace;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.opengl.GLES30;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import android.os.Handler;
import android.os.HandlerThread;

/** A real EGL producer into the ImageReader surface used by Flutter platform views. */
public final class ImageReaderEglTest {
    private static final int GL_PACK_REVERSE_ROW_ORDER_ANGLE = 0x93A4;
    private static final int GL_PIXEL_PACK_BUFFER = 0x88EB;
    private static final int GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED;
    private static final int GL_STREAM_READ = 0x88E1;

    private static boolean hasExtension(String list, String name) {
        return list != null && (" " + list + " ").contains(" " + name + " ");
    }


    // GLES3 child readback uses glGetStringi/GL_NUM_EXTENSIONS rather than
    // the GLES2 extension-string API. Run on ANGLE's ES3-capable adapters.
    private static void gles3ImageReader(EGLDisplay display) {
        final int EGL_OPENGL_ES3_BIT_KHR = 0x40;
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        boolean chosen = EGL14.eglChooseConfig(display,
                new int[] {EGL14.EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                        EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                        EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8,
                        EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                        EGL14.EGL_NONE}, 0, configs, 0, 1, count, 0) && count[0] == 1;
        if (!chosen) {
            System.out.println("INFO GLES3 ImageReader skipped (no GLES3 window config)");
            return;
        }
        EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE}, 0);
        if (context == EGL14.EGL_NO_CONTEXT) {
            System.out.println("INFO GLES3 ImageReader skipped (no GLES3 context)");
            return;
        }
        try (ImageReader reader = ImageReader.newInstance(8, 6, PixelFormat.RGBA_8888, 2)) {
            EGLSurface surface = EGL14.eglCreateWindowSurface(display, configs[0], reader.getSurface(),
                    new int[] {EGL14.EGL_NONE}, 0);
            check(surface != EGL14.EGL_NO_SURFACE, "GLES3 reader EGL surface");
            try {
                check(EGL14.eglMakeCurrent(display, surface, surface, context), "GLES3 reader current");
                int[] extensionCount = new int[1];
                GLES30.glGetIntegerv(GLES30.GL_NUM_EXTENSIONS, extensionCount, 0);
                check(extensionCount[0] >= 0, "GLES3 indexed extension count");
                GLES30.glPixelStorei(GLES30.GL_PACK_ALIGNMENT, 1);
                GLES30.glViewport(0, 0, 8, 6);
                GLES30.glClearColor(1f, 0f, 1f, 1f);
                GLES30.glClear(GLES30.GL_COLOR_BUFFER_BIT);
                check(EGL14.eglSwapBuffers(display, surface), "GLES3 child surface swap");
                int[] alignment = new int[1];
                GLES30.glGetIntegerv(GLES30.GL_PACK_ALIGNMENT, alignment, 0);
                check(alignment[0] == 1, "GLES3 pack alignment restored after child readback");
                check(GLES30.glGetError() == GLES30.GL_NO_ERROR, "GLES3 internal extension query has no GL errors");
                try (Image image = reader.acquireLatestImage()) {
                    check(image != null, "GLES3 ImageReader received frame");
                    ByteBuffer pixels = image.getPlanes()[0].getBuffer();
                    int index = 3 * image.getPlanes()[0].getRowStride()
                            + 4 * image.getPlanes()[0].getPixelStride();
                    check((pixels.get(index) & 0xff) == 255 &&
                            (pixels.get(index + 1) & 0xff) == 0 &&
                            (pixels.get(index + 2) & 0xff) == 255 &&
                            (pixels.get(index + 3) & 0xff) == 255,
                            "GLES3 ImageReader pixels remain RGBA");
                }
                GLES30.glPixelStorei(GLES30.GL_PACK_ALIGNMENT, 4);
            } finally {
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE,
                        EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                EGL14.eglDestroySurface(display, surface);
            }
        } finally {
            EGL14.eglDestroyContext(display, context);
        }
    }

    static void check(boolean value, String label) {
        if (!value) throw new AssertionError(label + " (EGL 0x" + Integer.toHexString(EGL14.eglGetError()) + ")");
        System.out.println("ImageReaderEglTest PASS " + label);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        check(EGL14.eglInitialize(display, new int[2], 0, new int[2], 0), "display");
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        check(EGL14.eglChooseConfig(display, new int[] { EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT, EGL14.EGL_RED_SIZE, 8,
                EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_NONE },
                0, configs, 0, 1, count, 0) && count[0] == 1, "config");
        EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE }, 0);
        check(context != EGL14.EGL_NO_CONTEXT, "context");
        // GPU_SAMPLED_IMAGE: the reader's HardwareBuffers are wrapped as Bitmaps below, and an
        // RGBA reader without that usage has CPU-only ones (as on Android).
        try (ImageReader reader = new ImageReader.Builder(16, 12).setMaxImages(2)
                .setImageFormat(PixelFormat.RGBA_8888).setUsage(HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE).build()) {
            check(reader.getSurface().isValid(), "native reader surface");
            EGLSurface surface = EGL14.eglCreateWindowSurface(display, configs[0], reader.getSurface(),
                    new int[] { EGL14.EGL_NONE }, 0);
            check(surface != EGL14.EGL_NO_SURFACE, "reader EGL surface");
            check(EGL14.eglMakeCurrent(display, surface, surface, context), "make current");
            // A child swap is an internal read: it must not change the app's
            // pack state. ANGLE can offer reverse rows and GLES2 PBOs as extensions.
            String extensions = GLES20.glGetString(GLES20.GL_EXTENSIONS);
            boolean reverse = hasExtension(extensions, "GL_ANGLE_pack_reverse_row_order");
            boolean pbo = hasExtension(extensions, "GL_NV_pixel_buffer_object")
                    || hasExtension(extensions, "GL_EXT_pixel_buffer_object");
            boolean packLayout = hasExtension(extensions, "GL_NV_pack_subimage");
            int[] packBuffer = new int[1];
            GLES20.glPixelStorei(GLES20.GL_PACK_ALIGNMENT, 1);
            if (reverse) GLES20.glPixelStorei(GL_PACK_REVERSE_ROW_ORDER_ANGLE, 1);
            if (packLayout) {
                // Legal GLES2 extension state must not affect the internal read.
                // The shorter row length also keeps a broken implementation's
                // overlapping writes within this test's native buffer.
                GLES20.glPixelStorei(GLES30.GL_PACK_ROW_LENGTH, 8);
                GLES20.glPixelStorei(GLES30.GL_PACK_SKIP_ROWS, 1);
                GLES20.glPixelStorei(GLES30.GL_PACK_SKIP_PIXELS, 2);
            }
            if (pbo) {
                GLES20.glGenBuffers(1, packBuffer, 0);
                GLES20.glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer[0]);
                GLES20.glBufferData(GL_PIXEL_PACK_BUFFER, 16 * 12 * 4, null, GL_STREAM_READ);
                check(packBuffer[0] != 0 && GLES20.glGetError() == GLES20.GL_NO_ERROR,
                        "PBO can be bound before child surface swap");
            }
            for (int color = 0; color < 3; color++) {
                GLES20.glViewport(0, 0, 16, 12);
                GLES20.glClearColor(color == 0 ? 1 : 0, color == 1 ? 1 : 0, color == 2 ? 1 : 0, 1);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                check(EGL14.eglSwapBuffers(display, surface), "swap " + color);
                int[] state = new int[1];
                GLES20.glGetIntegerv(GLES20.GL_PACK_ALIGNMENT, state, 0);
                check(state[0] == 1, "pack alignment unchanged after swap " + color);
                if (reverse) {
                    GLES20.glGetIntegerv(GL_PACK_REVERSE_ROW_ORDER_ANGLE, state, 0);
                    check(state[0] == 1, "reverse row order unchanged after swap " + color);
                }
                if (pbo) {
                    GLES20.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, state, 0);
                    check(state[0] == packBuffer[0], "PBO binding unchanged after swap " + color);
                }
                if (packLayout) {
                    GLES20.glGetIntegerv(GLES30.GL_PACK_ROW_LENGTH, state, 0);
                    check(state[0] == 8, "GLES2 pack row length unchanged after swap " + color);
                    GLES20.glGetIntegerv(GLES30.GL_PACK_SKIP_ROWS, state, 0);
                    check(state[0] == 1, "GLES2 pack skip rows unchanged after swap " + color);
                    GLES20.glGetIntegerv(GLES30.GL_PACK_SKIP_PIXELS, state, 0);
                    check(state[0] == 2, "GLES2 pack skip pixels unchanged after swap " + color);
                }
                check(GLES20.glGetError() == GLES20.GL_NO_ERROR,
                        "GLES state queries after swap have no errors " + color);
                try (Image image = reader.acquireLatestImage()) {
                    check(image != null, "image " + color);
                    check(image.getWidth() == 16 && image.getHeight() == 12 && image.getTimestamp() > 0, "metadata " + color);
                    Image.Plane plane = image.getPlanes()[0];
                    ByteBuffer bytes = plane.getBuffer();
                    for (int offset : new int[] {0,
                            5 * plane.getRowStride() + 7 * plane.getPixelStride(),
                            11 * plane.getRowStride() + 15 * plane.getPixelStride()}) {
                        for (int channel = 0; channel < 4; channel++)
                            check((bytes.get(offset + channel) & 255) == (channel == color || channel == 3 ? 255 : 0),
                                    "pixel " + color + "/" + offset + "/" + channel);
                    }
                    try (HardwareBuffer buffer = image.getHardwareBuffer()) {
                        check(buffer != null && buffer.getWidth() == 16 && buffer.getHeight() == 12
                                && buffer.getFormat() == HardwareBuffer.RGBA_8888
                                && buffer.getLayers() == 1 && buffer.getId() > 0
                                && (buffer.getUsage() & HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE) != 0, "reader buffer " + color);
                        Bitmap bitmap = Bitmap.wrapHardwareBuffer(buffer, ColorSpace.get(ColorSpace.Named.SRGB));
                        check(bitmap != null && bitmap.getPixel(7, 5) == new int[] {Color.RED, Color.GREEN, Color.BLUE}[color],
                                "wrapped bitmap pixel " + color);
                        buffer.close();
                        image.close();
                        check(bitmap.getPixel(7, 5) == new int[] {Color.RED, Color.GREEN, Color.BLUE}[color],
                                "bitmap owns pixels after image and buffer close " + color);
                        Bitmap target = Bitmap.createBitmap(16, 12, Bitmap.Config.ARGB_8888);
                        new Canvas(target).drawBitmap(bitmap, 0, 0, null);
                        check(target.getPixel(7, 5) == new int[] {Color.RED, Color.GREEN, Color.BLUE}[color],
                                "Flutter-style Canvas draw " + color);
                        bitmap.recycle(); target.recycle();
                    }
                }
            }
            if (pbo) {
                GLES20.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                GLES20.glDeleteBuffers(1, packBuffer, 0);
            }
            if (reverse) GLES20.glPixelStorei(GL_PACK_REVERSE_ROW_ORDER_ANGLE, 0);
            if (packLayout) {
                GLES20.glPixelStorei(GLES30.GL_PACK_ROW_LENGTH, 0);
                GLES20.glPixelStorei(GLES30.GL_PACK_SKIP_ROWS, 0);
                GLES20.glPixelStorei(GLES30.GL_PACK_SKIP_PIXELS, 0);
            }
            GLES20.glPixelStorei(GLES20.GL_PACK_ALIGNMENT, 4);
            HandlerThread callbacks = new HandlerThread("reader-callback");
            callbacks.start();
            try {
                CountDownLatch notified = new CountDownLatch(1);
                reader.setOnImageAvailableListener(ignored -> notified.countDown(), new Handler(callbacks.getLooper()));
                for (int i = 0; i < 5; i++) {
                    Canvas canvas = reader.getSurface().lockCanvas(null);
                    canvas.drawColor(i == 4 ? Color.MAGENTA : Color.YELLOW);
                    reader.getSurface().unlockCanvasAndPost(canvas);
                }
                check(notified.await(3, TimeUnit.SECONDS), "native frame notification");
                try (Image image = reader.acquireLatestImage()) {
                    check(image != null && image.getPlanes()[0].getBuffer().get(0) == (byte)255
                            && image.getPlanes()[0].getBuffer().get(1) == 0
                            && image.getPlanes()[0].getBuffer().get(2) == (byte)255, "latest frame after queue overflow");
                }
                check(reader.acquireNextImage() == null, "latest acquire drains queue");
                reader.close();
                // EGL owns its producer reference until destroy, even after the reader closes.
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                check(EGL14.eglSwapBuffers(display, surface), "closed reader tolerates retained EGL producer");
            } finally {
                callbacks.quitSafely();
                callbacks.join(3000);
                check(!callbacks.isAlive(), "callback thread stopped");
            }
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            check(EGL14.eglDestroySurface(display, surface), "destroy surface");
        }
        check(EGL14.eglDestroyContext(display, context), "destroy context");
        try (ImageReader reader = ImageReader.newInstance(4, 4, ImageFormat.YUV_420_888, 2)) {
            byte[] nv12 = new byte[24];
            Arrays.fill(nv12, (byte)128);
            Class<?> frames = Class.forName("com.gesher.server.CameraFrames");
            int delivered = (Integer)frames.getMethod("deliver", android.view.Surface.class,
                    byte[].class, int.class, int.class, long.class, byte[].class)
                    .invoke(null, reader.getSurface(), nv12, 4, 4, 12345L, null);
            check(delivered == 0, "direct camera delivery preserved");
            try (Image image = reader.acquireNextImage()) {
                check(image != null && image.getPlanes().length == 3 && image.getTimestamp() == 12345L
                        && (image.getPlanes()[0].getBuffer().get(0) & 255) == 128, "direct camera YUV pixels");
            }
        }
        gles3ImageReader(display);
        System.out.println("ImageReaderEglTest all passed");
    }
}
