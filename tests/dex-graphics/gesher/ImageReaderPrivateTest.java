/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ImageFormat;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.hardware.HardwareBuffer;
import android.hardware.SyncFence;
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
import android.os.ParcelFileDescriptor;
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
 * frames, which a PRIVATE reader also takes, arrive as RGBA like drawn ones.
 */
public final class ImageReaderPrivateTest {
    private static final int W = 32, H = 24;
    // RGBA bytes read as a little-endian int.
    private static final int RED = 0xff0000ff, GREEN = 0xff00ff00, BLUE = 0xffff0000;

    private static native int nativeInspect(HardwareBuffer buffer, int[] probes, int[] out);
    private static native boolean nativeCheckJavaBuffer(HardwareBuffer buffer, int width,
            int height, int format, long usage, long id);
    private static native int nativeFenceFd();
    // An EGLImage and texture kept after the Image and HardwareBuffer are closed
    // (tests/native/reader_buffer_test.c).
    private static native int nativeKeep(HardwareBuffer buffer, int[] out);
    private static native int nativeSampleKept(int[] probes, int[] out);
    private static native int nativeReadTarget(int[] probes, int[] out);
    private static native void nativeDropKept();
    private static native void nativeFinish();
    private static native long nativeFenceStats(int which);

    private static final int[] PROBES = {4, 4, 28, 4, 4, 20, 28, 20};
    // Indexes of nativeInspect's result.
    private static final int STATUS = 0, WIDTH = 1, HEIGHT = 2, FORMAT = 3, USAGE = 4, SAME = 5,
            IMAGE = 6, ID_LO = 7, ID_HI = 8, USAGE_HI = 9, GL_2D = 10, GL_EXT = 11, PIXELS = 12;
    private static final int N = PROBES.length / 2;
    // nativeInspect's result: the fixed values, then the CPU, GL_TEXTURE_2D and
    // GL_TEXTURE_EXTERNAL_OES pixels of PROBES.
    private static final int RESULT = PIXELS + 3 * N;

    static void check(boolean value, String label) {
        if (!value) throw new AssertionError(label);
        System.out.println("ImageReaderPrivateTest PASS " + label);
    }

    private static void checkJavaAllocation() {
        long cpu = HardwareBuffer.USAGE_CPU_READ_OFTEN | HardwareBuffer.USAGE_CPU_WRITE_OFTEN;
        for (int format : new int[] {HardwareBuffer.RGBA_8888, HardwareBuffer.RGB_565,
                HardwareBuffer.BLOB}) {
            int height = format == HardwareBuffer.BLOB ? 1 : 5;
            check(HardwareBuffer.isSupported(7, height, format, 1, cpu),
                    "Java allocation supported for format " + format);
            try (HardwareBuffer buffer = HardwareBuffer.create(7, height, format, 1, cpu)) {
                check(buffer.getWidth() == 7 && buffer.getHeight() == height
                        && buffer.getFormat() == format && buffer.getLayers() == 1
                        && buffer.getUsage() == cpu, "Java allocation metadata for format " + format);
                check(nativeCheckJavaBuffer(buffer, 7, height, format, cpu, buffer.getId())
                        && buffer.isClosed(), "native reference outlives Java close for format " + format);
            }
        }

        long invalid = HardwareBuffer.USAGE_CPU_READ_OFTEN | HardwareBuffer.USAGE_PROTECTED_CONTENT;
        check(!HardwareBuffer.isSupported(7, 5, HardwareBuffer.RGBA_8888, 1, invalid),
                "Java query rejects a CPU-readable protected buffer");
        boolean rejected = false;
        try (HardwareBuffer buffer = HardwareBuffer.create(7, 5, HardwareBuffer.RGBA_8888, 1, invalid)) {
            check(false, "unexpected protected allocation " + buffer);
        } catch (IllegalArgumentException expected) {
            rejected = true;
        }
        check(rejected, "Java create throws IllegalArgumentException for an unsupported buffer");
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
            int[] out = new int[RESULT];
            check(nativeInspect(buffer, PROBES, out) == 0, label + ": AHardwareBuffer_fromHardwareBuffer");
            check(out[WIDTH] == W && out[HEIGHT] == H, label + ": AHardwareBuffer_describe size");
            check(out[FORMAT] == 1, label + ": AHardwareBuffer_describe format R8G8B8A8");
            check((out[USAGE] & 0xF) == 3 && (out[USAGE] & HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE) != 0,
                    label + ": AHardwareBuffer_describe usage is CPU_READ_OFTEN | GPU_SAMPLED_IMAGE");
            long nativeId = (out[ID_HI] & 0xffffffffL) << 32 | (out[ID_LO] & 0xffffffffL);
            long nativeUsage = (out[USAGE_HI] & 0xffffffffL) << 32 | (out[USAGE] & 0xffffffffL);
            check(buffer.getId() == nativeId && nativeId > 0,
                    label + ": HardwareBuffer.getId() is AHardwareBuffer_getId (" + nativeId + ")");
            check(buffer.getUsage() == nativeUsage,
                    label + ": HardwareBuffer.getUsage() is AHardwareBuffer_describe's usage");
            check(out[SAME] == 1, label + ": a second fromHardwareBuffer is the same buffer");
            for (int i = 0; i < expect.length; i++)
                check(out[PIXELS + i] == expect[i], label + ": pixel " + i + " 0x"
                        + Integer.toHexString(out[PIXELS + i]) + " is 0x" + Integer.toHexString(expect[i]));
            if (gl) {
                check(out[IMAGE] == 1, label + ": eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) of the buffer");
                check(out[GL_2D] == 1, label + ": the EGLImage binds to a GL_TEXTURE_2D that a framebuffer reads");
                check(out[GL_EXT] == 1, label + ": a samplerExternalOES shader samples the EGLImage");
                for (int i = 0; i < expect.length; i++) {
                    check(out[PIXELS + N + i] == expect[i], label + ": texture pixel " + i + " 0x"
                            + Integer.toHexString(out[PIXELS + N + i]) + " is 0x" + Integer.toHexString(expect[i]));
                    check(out[PIXELS + 2 * N + i] == expect[i], label + ": external-sampled pixel " + i + " 0x"
                            + Integer.toHexString(out[PIXELS + 2 * N + i]) + " is 0x" + Integer.toHexString(expect[i]));
                }
            }
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        System.load(args[0]);
        checkJavaAllocation();
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
                try (SyncFence fence = image.getFence()) {
                    check(!fence.isValid(), "a reader image's fence is the empty one");
                    fence.awaitForever();  // false for an empty fence, but it must return
                    check(fence.getSignalTime() == SyncFence.SIGNAL_TIME_INVALID, "empty fence signal time");
                }
                inspect(image, new int[] {RED, BLUE, RED, BLUE}, false, "lockHardwareCanvas");

                // The buffer keeps its own pixels: closing the image does not change what an
                // imported buffer sees, and a later frame is another buffer.
                HardwareBuffer held = image.getHardwareBuffer();
                image.close();
                int[] out = new int[RESULT];
                check(nativeInspect(held, PROBES, out) == 0 && out[PIXELS] == RED
                        && out[PIXELS + 1] == BLUE, "the buffer outlives its image");
                latch[0] = new CountDownLatch(1);
                canvas = reader.getSurface().lockHardwareCanvas();
                canvas.drawColor(Color.GREEN);
                reader.getSurface().unlockCanvasAndPost(canvas);
                image = next(reader, latch[0], "second hardware frame");
                inspect(image, new int[] {GREEN, GREEN, GREEN, GREEN}, false, "second hardware frame");
                check(nativeInspect(held, PROBES, out) == 0 && out[PIXELS] == RED,
                        "the earlier buffer still holds its own frame");
                image.close();
                held.close();
                check(nativeInspect(held, PROBES, out) != 0,
                        "a closed HardwareBuffer is not imported");
            }

            // --- consecutive lockHardwareCanvas frames on one reader (Flutter's
            // PlatformViewWrapper.draw does this for every frame) ----
            try (ImageReader reader = privateReader(2)) {
                final int[] colors = {Color.RED, Color.GREEN, Color.BLUE, Color.YELLOW, Color.MAGENTA, Color.CYAN,
                        0xff804020, 0xff102030, Color.RED, Color.WHITE};
                final int[] seen = new int[1];
                CountDownLatch[] latch = {new CountDownLatch(1)};
                reader.setOnImageAvailableListener(r -> { seen[0]++; latch[0].countDown(); }, handler);
                for (int frame = 0; frame < colors.length; frame++) {
                    latch[0] = new CountDownLatch(1);
                    Canvas canvas = reader.getSurface().lockHardwareCanvas();
                    canvas.drawColor(colors[frame]);
                    reader.getSurface().unlockCanvasAndPost(canvas);
                    Image image = next(reader, latch[0], "hardware frame " + frame);
                    int c = colors[frame];
                    int px = (c & 0xff00ff00) | ((c & 0xff) << 16) | ((c >> 16) & 0xff);
                    inspect(image, new int[] {px, px, px, px}, false, "hardware frame " + frame);
                    image.close();
                }
                check(seen[0] == colors.length, "one notification per frame (" + seen[0] + ")");
            }

            // --- the buffer released longest ago is the one rewritten ---------------------
            try (ImageReader reader = privateReader(2)) {
                CountDownLatch[] latch = {new CountDownLatch(1)};
                reader.setOnImageAvailableListener(r -> latch[0].countDown(), handler);
                long[] ids = new long[2];
                Image[] held = new Image[2];
                for (int i = 0; i < 2; i++) {  // two frames held at once: two buffers
                    latch[0] = new CountDownLatch(1);
                    Canvas canvas = reader.getSurface().lockCanvas(null);
                    canvas.drawColor(Color.RED);
                    reader.getSurface().unlockCanvasAndPost(canvas);
                    held[i] = next(reader, latch[0], "lru held frame " + i);
                    try (HardwareBuffer b = held[i].getHardwareBuffer()) {
                        ids[i] = b.getId();
                    }
                }
                check(ids[0] != ids[1], "two held frames are two buffers");
                held[0].close();
                held[1].close();
                long previous = 0;
                for (int frame = 0; frame < 6; frame++) {
                    latch[0] = new CountDownLatch(1);
                    Canvas canvas = reader.getSurface().lockCanvas(null);
                    canvas.drawColor(Color.GREEN);
                    reader.getSurface().unlockCanvasAndPost(canvas);
                    try (Image image = next(reader, latch[0], "lru frame " + frame);
                            HardwareBuffer b = image.getHardwareBuffer()) {
                        check(b.getId() == ids[0] || b.getId() == ids[1], "lru frame " + frame + " is a pooled buffer");
                        check(b.getId() != previous, "lru frame " + frame
                                + " is not the buffer just released (" + b.getId() + ")");
                        previous = b.getId();
                    }
                }
            }

            // --- software Canvas (a Surface does not mix it with lockHardwareCanvas) ----
            try (ImageReader reader = privateReader(2)) {
                CountDownLatch[] latch = {new CountDownLatch(1)};
                reader.setOnImageAvailableListener(r -> latch[0].countDown(), handler);
                Canvas canvas = reader.getSurface().lockCanvas(null);
                canvas.drawColor(Color.RED);
                reader.getSurface().unlockCanvasAndPost(canvas);
                Image image = next(reader, latch[0], "lockCanvas");
                inspect(image, new int[] {RED, RED, RED, RED}, false, "lockCanvas");
                HardwareBuffer held = image.getHardwareBuffer();
                image.close();
                long heldId = held.getId();
                // Buffers repeat across frames, but a held one is never overwritten.
                java.util.HashSet<Long> ids = new java.util.HashSet<>();
                for (int frame = 0; frame < 8; frame++) {
                    latch[0] = new CountDownLatch(1);
                    canvas = reader.getSurface().lockCanvas(null);
                    canvas.drawColor(frame % 2 == 0 ? Color.YELLOW : Color.MAGENTA);
                    reader.getSurface().unlockCanvasAndPost(canvas);
                    try (Image frameImage = next(reader, latch[0], "pooled frame " + frame);
                            HardwareBuffer frameBuffer = frameImage.getHardwareBuffer()) {
                        check(frameBuffer.getId() != heldId, "frame " + frame + " is not the held buffer");
                        ids.add(frameBuffer.getId());
                    }
                }
                check(ids.size() <= 3, "buffer ids repeat across frames (" + ids.size() + " distinct in 8)");
                int[] out = new int[RESULT];
                check(nativeInspect(held, PROBES, out) == 0 && out[PIXELS] == RED,
                        "the held buffer kept its frame while others were drawn");
                held.close();
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
                // An asymmetric frame, so a flip or a red/blue swap shows: blue, with
                // GL's bottom-left quadrant (the image's bottom-left) red. PROBES are the
                // image's top-left, top-right, bottom-left and bottom-right.
                GLES20.glViewport(0, 0, W, H);
                GLES20.glClearColor(0f, 0f, 1f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
                GLES20.glScissor(0, 0, W / 2, H / 2);
                GLES20.glClearColor(1f, 0f, 0f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
                check(EGL14.eglSwapBuffers(display, surface), "EGL swap into the PRIVATE reader");
                Image image = next(reader, latch, "EGL");
                inspect(image, new int[] {BLUE, BLUE, RED, BLUE}, true, "EGL quadrant");
                image.close();
                // The next frame (in the same pooled buffer, whose texture exists by now):
                // the red quadrant moves to the top-right and the new pixels reach the GPU.
                CountDownLatch second = new CountDownLatch(1);
                reader.setOnImageAvailableListener(r -> second.countDown(), handler);
                GLES20.glClearColor(0f, 1f, 0f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
                GLES20.glScissor(W / 2, H / 2, W / 2, H / 2);
                GLES20.glClearColor(1f, 0f, 0f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
                check(EGL14.eglSwapBuffers(display, surface), "second EGL swap");
                image = next(reader, second, "EGL second frame");
                inspect(image, new int[] {GREEN, RED, GREEN, GREEN}, true, "EGL second frame");
                // A descriptor-backed SyncFence over a native fence of this display.
                int fd = nativeFenceFd();
                check(fd >= 0, "a native fence descriptor from eglDupNativeFenceFDANDROID");
                try (SyncFence fence = SyncFence.create(ParcelFileDescriptor.adoptFd(fd))) {
                    check(fence.isValid(), "a descriptor-backed SyncFence is valid");
                    check(fence.awaitForever(), "it signals");
                    check(fence.getSignalTime() == SyncFence.SIGNAL_TIME_INVALID,
                            "a signaled fence has no recorded signal time");
                }
                image.close();
                // --- an EGLImage kept after its Image and HardwareBuffer are closed (Flutter's
                // texture cache hit): the buffer is reused at once, the earlier draw that
                // sampled it stays ordered before the rewrite, and the kept texture then
                // samples the new frame.
                try (ImageReader keptReader = privateReader(2)) {
                    CountDownLatch[] kl = {new CountDownLatch(1)};
                    keptReader.setOnImageAvailableListener(r -> kl[0].countDown(), handler);
                    Canvas kc = keptReader.getSurface().lockHardwareCanvas();
                    kc.drawColor(Color.RED);
                    keptReader.getSurface().unlockCanvasAndPost(kc);
                    Image first = next(keptReader, kl[0], "kept: first frame");
                    HardwareBuffer firstBuffer = first.getHardwareBuffer();
                    long firstId = firstBuffer.getId();
                    int[] keep = new int[3];
                    check(nativeKeep(firstBuffer, keep) == 0,
                            "kept: EGLImage, texture and a queued draw of the first frame");
                    check(((keep[2] & 0xffffffffL) << 32 | (keep[1] & 0xffffffffL)) == firstId,
                            "kept: the native id is the Java id");
                    // Closed at once, with no glFinish: the draw may still be queued.
                    firstBuffer.close();
                    first.close();
                    kl[0] = new CountDownLatch(1);
                    kc = keptReader.getSurface().lockHardwareCanvas();
                    kc.drawColor(Color.GREEN);
                    keptReader.getSurface().unlockCanvasAndPost(kc);
                    Image secondImage = next(keptReader, kl[0], "kept: second frame");
                    HardwareBuffer secondBuffer = secondImage.getHardwareBuffer();  // uploads the frame
                    check(secondBuffer.getId() == firstId,
                            "kept: the buffer is reused while an EGLImage of it is alive");
                    int[] earlier = new int[N];
                    check(nativeReadTarget(PROBES, earlier) == 0, "kept: read the earlier draw");
                    for (int i = 0; i < N; i++)
                        check(earlier[i] == RED, "kept: the draw queued before the rewrite sampled the old frame, pixel "
                                + i + " 0x" + Integer.toHexString(earlier[i]));
                    int[] now = new int[N];
                    check(nativeSampleKept(PROBES, now) == 0, "kept: sample the kept texture");
                    for (int i = 0; i < N; i++)
                        check(now[i] == GREEN, "kept: the kept texture samples the new frame, pixel " + i
                                + " 0x" + Integer.toHexString(now[i]));
                    secondBuffer.close();
                    secondImage.close();
                    // Steady state: the rewrite waits for the buffer's release fence, not for the
                    // device. With the app's draws finished (and the fence given a moment to
                    // signal) before each next frame, none of these rewrites finds it pending.
                    final int steady = 6;
                    long waits = nativeFenceStats(0), pending = nativeFenceStats(1);
                    check(waits >= 0 && pending >= 0, "gesher_ahb_fence_stats is available");
                    int[] colors = {Color.RED, Color.BLUE, Color.YELLOW, Color.MAGENTA, Color.CYAN, Color.GREEN};
                    for (int i = 0; i < steady; i++) {
                        nativeFinish();
                        Thread.sleep(50);
                        kl[0] = new CountDownLatch(1);
                        kc = keptReader.getSurface().lockHardwareCanvas();
                        kc.drawColor(colors[i]);
                        keptReader.getSurface().unlockCanvasAndPost(kc);
                        try (Image frame = next(keptReader, kl[0], "steady frame " + i);
                                HardwareBuffer frameBuffer = frame.getHardwareBuffer()) {
                            check(frameBuffer.getId() == firstId, "steady frame " + i + " reuses the kept buffer");
                        }
                    }
                    long waited = nativeFenceStats(0) - waits, blocked = nativeFenceStats(1) - pending;
                    check(waited == steady, "each steady frame's rewrite waited on its release fence (" + waited + ")");
                    check(blocked == 0, "no steady frame's rewrite found the fence pending, so none drained the "
                            + "device (" + blocked + " of " + steady + ")");
                    nativeDropKept();
                }
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
                    // NV12 grey 128/128/128 as RGBA (BT.601 limited range): 130 each.
                    inspect(image, new int[] {0xff828282, 0xff828282, 0xff828282, 0xff828282},
                            false, "camera frame");
                }
            }
            // --- a reader nothing samples on the GPU has CPU-only buffers (a camera QR
            // scanner's), a GPU-sampled one GPU-capable ones ------------------------------
            for (boolean gpu : new boolean[] {false, true}) {
                ImageReader.Builder builder = new ImageReader.Builder(W, H).setMaxImages(2)
                        .setImageFormat(PixelFormat.RGBA_8888);
                if (gpu) builder.setUsage(HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE);
                try (ImageReader reader = builder.build()) {
                    byte[] nv12 = new byte[W * H * 3 / 2];
                    Arrays.fill(nv12, (byte) 128);
                    Class<?> frames = Class.forName("com.gesher.server.CameraFrames");
                    check((Integer) frames.getMethod("deliver", Surface.class, byte[].class,
                            int.class, int.class, long.class, byte[].class)
                            .invoke(null, reader.getSurface(), nv12, W, H, 1L, null) == 0,
                            "camera delivery into a " + (gpu ? "GPU-sampled" : "CPU") + " RGBA reader");
                    try (Image image = reader.acquireNextImage(); HardwareBuffer buffer = image.getHardwareBuffer()) {
                        long usage = buffer.getUsage();
                        check((usage & HardwareBuffer.USAGE_CPU_READ_OFTEN) == HardwareBuffer.USAGE_CPU_READ_OFTEN,
                                "camera RGBA buffer is CPU readable");
                        check(((usage & HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE) != 0) == gpu,
                                "camera RGBA buffer reports its true usage 0x" + Long.toHexString(usage)
                                        + (gpu ? " (GPU sampled)" : " (CPU only)"));
                    }
                }
            }
        } finally {
            callbacks.quitSafely();
            callbacks.join(3000);
        }
        System.out.println("ImageReaderPrivateTest all passed");
    }
}
