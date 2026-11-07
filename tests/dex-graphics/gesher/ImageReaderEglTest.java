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
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import android.os.Handler;
import android.os.HandlerThread;

/** A real EGL producer into the ImageReader surface used by Flutter platform views. */
public final class ImageReaderEglTest {
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
        try (ImageReader reader = ImageReader.newInstance(16, 12, PixelFormat.RGBA_8888, 2)) {
            check(reader.getSurface().isValid(), "native reader surface");
            EGLSurface surface = EGL14.eglCreateWindowSurface(display, configs[0], reader.getSurface(),
                    new int[] { EGL14.EGL_NONE }, 0);
            check(surface != EGL14.EGL_NO_SURFACE, "reader EGL surface");
            check(EGL14.eglMakeCurrent(display, surface, surface, context), "make current");
            for (int color = 0; color < 3; color++) {
                GLES20.glViewport(0, 0, 16, 12);
                GLES20.glClearColor(color == 0 ? 1 : 0, color == 1 ? 1 : 0, color == 2 ? 1 : 0, 1);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                check(EGL14.eglSwapBuffers(display, surface), "swap " + color);
                try (Image image = reader.acquireLatestImage()) {
                    check(image != null, "image " + color);
                    check(image.getWidth() == 16 && image.getHeight() == 12 && image.getTimestamp() > 0, "metadata " + color);
                    Image.Plane plane = image.getPlanes()[0];
                    ByteBuffer bytes = plane.getBuffer();
                    int offset = 5 * plane.getRowStride() + 7 * plane.getPixelStride();
                    for (int channel = 0; channel < 4; channel++)
                        check((bytes.get(offset + channel) & 255) == (channel == color || channel == 3 ? 255 : 0),
                                "pixel " + color + "/" + channel);
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
        System.out.println("ImageReaderEglTest all passed");
    }
}
