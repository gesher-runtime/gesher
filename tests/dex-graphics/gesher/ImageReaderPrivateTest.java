/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ImageFormat;
import android.graphics.Paint;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * A PRIVATE, GPU-sampled ImageReader as Flutter's platform views create it
 * (ImageReader.Builder, PRIVATE, GPU_SAMPLED_IMAGE): an app draws into its Surface
 * (lockHardwareCanvas, lockCanvas, EGL), acquires the image and imports its
 * HardwareBuffer through the NDK (AHardwareBuffer_fromHardwareBuffer,
 * tests/native/reader_buffer_test.c, the library given as the argument). Camera
 * frames, which a PRIVATE reader also takes, keep their own path.
 */
public final class ImageReaderPrivateTest {
    private static final int W = 32, H = 24;
    // RGBA bytes read as a little-endian int.
    private static final int RED = 0xff0000ff, GREEN = 0xff00ff00, BLUE = 0xffff0000;

    private static native int nativeInspect(HardwareBuffer buffer, int[] probes, int[] out);

    private static final int[] PROBES = {4, 4, 28, 4, 4, 20, 28, 20};
    // Indexes of nativeInspect's result.
    private static final int STATUS = 0, WIDTH = 1, HEIGHT = 2, FORMAT = 3, USAGE = 4, SAME = 5,
            IMAGE = 6, PIXELS = 7;

    static void check(boolean value, String label) {
        if (!value) throw new AssertionError(label);
        System.out.println("ImageReaderPrivateTest PASS " + label);
    }

    private static ImageReader privateReader(int maxImages) {
        // What Flutter's ImageReaderSurfaceProducer.createImageReader33 builds.
        return new ImageReader.Builder(W, H).setMaxImages(maxImages)
                .setImageFormat(ImageFormat.PRIVATE)
                .setUsage(HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE).build();
    }

    private static Image next(ImageReader reader, CountDownLatch available, String label)
            throws Exception {
        check(available.await(5, TimeUnit.SECONDS), label + ": image available notification");
        Image image = reader.acquireLatestImage();
        check(image != null, label + ": acquired an image");
        return image;
    }

    /** Imports the image's buffer natively and checks its description and pixels. */
    private static void inspect(Image image, int[] expect, boolean gl, String label) {
        try (HardwareBuffer buffer = image.getHardwareBuffer()) {
            check(buffer != null, label + ": getHardwareBuffer");
            check(buffer.getWidth() == W && buffer.getHeight() == H
                    && buffer.getFormat() == HardwareBuffer.RGBA_8888 && buffer.getLayers() == 1
                    && (buffer.getUsage() & HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE) != 0,
                    label + ": Java HardwareBuffer description");
            int[] out = new int[PIXELS + PROBES.length / 2];
            check(nativeInspect(buffer, PROBES, out) == 0, label + ": AHardwareBuffer_fromHardwareBuffer");
            check(out[WIDTH] == W && out[HEIGHT] == H, label + ": AHardwareBuffer_describe size");
            check(out[FORMAT] == 1, label + ": AHardwareBuffer_describe format R8G8B8A8");
            check((out[USAGE] & HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE) != 0
                    && (out[USAGE] & HardwareBuffer.USAGE_CPU_READ_OFTEN) != 0,
                    label + ": AHardwareBuffer_describe usage samples on the GPU");
            check(out[SAME] == 1, label + ": a second fromHardwareBuffer is the same buffer");
            for (int i = 0; i < expect.length; i++)
                check(out[PIXELS + i] == expect[i], label + ": pixel " + i + " 0x"
                        + Integer.toHexString(out[PIXELS + i]) + " is 0x" + Integer.toHexString(expect[i]));
            if (gl) check(out[IMAGE] == 1, label + ": eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) of the buffer");
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        System.load(args[0]);
        HandlerThread callbacks = new HandlerThread("reader-private-callback");
        callbacks.start();
        Handler handler = new Handler(callbacks.getLooper());
        try {
            // --- Canvas producers -----------------------------------------------------
            try (ImageReader reader = privateReader(2)) {
                check(reader.getImageFormat() == ImageFormat.PRIVATE, "reader format is PRIVATE");
                check(reader.getSurface() != null && reader.getSurface().isValid(),
                        "a PRIVATE reader has a native Surface");
                check(reader.getMaxImages() == 2, "max images");
                CountDownLatch[] latch = {new CountDownLatch(1)};
                reader.setOnImageAvailableListener(r -> latch[0].countDown(), handler);

                // lockHardwareCanvas: the call Flutter makes to clear the reader.
                Canvas canvas = reader.getSurface().lockHardwareCanvas();
                Paint paint = new Paint();
                paint.setColor(Color.RED);
                canvas.drawRect(0, 0, W / 2f, H, paint);
                paint.setColor(Color.BLUE);
                canvas.drawRect(W / 2f, 0, W, H, paint);
                reader.getSurface().unlockCanvasAndPost(canvas);
                Image image = next(reader, latch[0], "lockHardwareCanvas");
                check(image.getFormat() == ImageFormat.PRIVATE, "Image.getFormat stays PRIVATE");
                check(image.getWidth() == W && image.getHeight() == H && image.getTimestamp() >= 0,
                        "image size");
                check(image.getPlanes().length == 0, "a PRIVATE image has no CPU planes");
                inspect(image, new int[] {RED, BLUE, RED, BLUE}, false, "lockHardwareCanvas");

                // The buffer keeps its own pixels: closing the image does not change what an
                // imported buffer sees, and a later frame is another buffer.
                HardwareBuffer held = image.getHardwareBuffer();
                image.close();
                int[] out = new int[PIXELS + PROBES.length / 2];
                check(nativeInspect(held, PROBES, out) == 0 && out[PIXELS] == RED
                        && out[PIXELS + 1] == BLUE, "the buffer outlives its image");
                latch[0] = new CountDownLatch(1);
                canvas = reader.getSurface().lockCanvas(null);
                canvas.drawColor(Color.GREEN);
                reader.getSurface().unlockCanvasAndPost(canvas);
                image = next(reader, latch[0], "lockCanvas");
                inspect(image, new int[] {GREEN, GREEN, GREEN, GREEN}, false, "lockCanvas");
                check(nativeInspect(held, PROBES, out) == 0 && out[PIXELS] == RED,
                        "the earlier buffer still holds its own frame");
                image.close();
                held.close();
                check(nativeInspect(held, PROBES, out) != 0,
                        "a closed HardwareBuffer is not imported");
            }

            // --- EGL producer, as the platform-view render target draws ---------------
            EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            check(EGL14.eglInitialize(display, new int[2], 0, new int[2], 0), "EGL display");
            EGLConfig[] configs = new EGLConfig[1];
            int[] count = new int[1];
            check(EGL14.eglChooseConfig(display, new int[] {EGL14.EGL_RENDERABLE_TYPE,
                    EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                    EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                    EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_NONE}, 0, configs, 0, 1, count, 0)
                    && count[0] == 1, "EGL config");
            EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                    new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            check(context != EGL14.EGL_NO_CONTEXT, "EGL context");
            try (ImageReader reader = privateReader(2)) {
                CountDownLatch latch = new CountDownLatch(1);
                reader.setOnImageAvailableListener(r -> latch.countDown(), handler);
                EGLSurface surface = EGL14.eglCreateWindowSurface(display, configs[0],
                        reader.getSurface(), new int[] {EGL14.EGL_NONE}, 0);
                check(surface != EGL14.EGL_NO_SURFACE, "EGL window surface on the PRIVATE reader");
                check(EGL14.eglMakeCurrent(display, surface, surface, context), "EGL make current");
                GLES20.glViewport(0, 0, W, H);
                GLES20.glClearColor(0f, 1f, 0f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                check(EGL14.eglSwapBuffers(display, surface), "EGL swap into the PRIVATE reader");
                Image image = next(reader, latch, "EGL");
                inspect(image, new int[] {GREEN, GREEN, GREEN, GREEN}, true, "EGL");
                image.close();
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
                        EGL14.EGL_NO_CONTEXT);
                EGL14.eglDestroySurface(display, surface);
            }
            EGL14.eglDestroyContext(display, context);

            // --- camera frames in a PRIVATE reader -------------------------------------
            try (ImageReader reader = privateReader(2)) {
                byte[] nv12 = new byte[W * H * 3 / 2];
                Arrays.fill(nv12, (byte) 128);
                Class<?> frames = Class.forName("com.gesher.server.CameraFrames");
                Surface surface = reader.getSurface();
                check((Boolean) frames.getMethod("isReader", Surface.class).invoke(null, surface),
                        "the camera finds a PRIVATE reader by its Surface");
                int delivered = (Integer) frames.getMethod("deliver", Surface.class, byte[].class,
                        int.class, int.class, long.class, byte[].class)
                        .invoke(null, surface, nv12, W, H, 4242L, null);
                check(delivered == 0, "camera delivery into a PRIVATE reader");
                try (Image image = reader.acquireNextImage()) {
                    check(image != null && image.getTimestamp() == 4242L
                            && image.getFormat() == ImageFormat.PRIVATE
                            && image.getPlanes().length == 0, "camera PRIVATE image");
                    check(image.getHardwareBuffer() == null,
                            "a camera frame has no RGBA snapshot to hand out");
                }
            }
        } finally {
            callbacks.quitSafely();
            callbacks.join(3000);
        }
        System.out.println("ImageReaderPrivateTest all passed");
    }
}
