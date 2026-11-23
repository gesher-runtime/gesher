/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.recordapp;

import android.app.Activity;
import android.graphics.Rect;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaRecorder;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

import java.io.File;
import java.io.FileDescriptor;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Real public API: one EGL producer/recorder, three MP4 files, every decoded frame identified. */
public final class RecorderRolloverActivity extends Activity {
    private static final int WIDTH = 128, HEIGHT = 96;
    private static final long FILE_LIMIT = 8192;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }
    private static void say(String text) {
        System.out.println("recorder rollover: " + text);
        System.out.flush();
    }
    private interface Checked { void run() throws Exception; }
    private static void state(Checked action) throws Exception {
        try { action.run(); throw new AssertionError("missing IllegalStateException"); }
        catch (IllegalStateException expected) {}
    }
    private static void argument(Checked action) throws Exception {
        try { action.run(); throw new AssertionError("missing IllegalArgumentException"); }
        catch (IllegalArgumentException expected) {}
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        // Run here, in onCreate, like RecordActivity: the single-process launch
        // (com.gesher.app.Main --activity) ends the VM once launch returns, so a
        // background thread would be cut off before it reports. Recorder
        // callbacks arrive on exercise()'s own HandlerThread, never on this
        // looper, so nothing here waits for the main thread.
        exercise();
    }

    private static final class Producer implements AutoCloseable {
        private EGLDisplay display = EGL14.EGL_NO_DISPLAY;
        private EGLContext context = EGL14.EGL_NO_CONTEXT;
        private EGLSurface window = EGL14.EGL_NO_SURFACE;
        private static void egl(boolean result, String operation) {
            require(result, operation + " EGL error 0x" + Integer.toHexString(EGL14.eglGetError()));
        }
        Producer(Surface surface) {
            try {
                display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
                egl(display != EGL14.EGL_NO_DISPLAY, "eglGetDisplay");
                int[] version = new int[2];
                egl(EGL14.eglInitialize(display, version, 0, version, 1), "eglInitialize");
                EGLConfig[] configs = new EGLConfig[1];
                int[] count = new int[1];
                egl(EGL14.eglChooseConfig(display, new int[]{EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT, EGL14.EGL_RED_SIZE, 8,
                    EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                    0x3142 /* EGL_RECORDABLE_ANDROID */, 1, EGL14.EGL_NONE}, 0, configs, 0, 1, count, 0)
                    && count[0] == 1, "eglChooseConfig");
                context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                    new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
                egl(context != EGL14.EGL_NO_CONTEXT, "eglCreateContext");
                window = EGL14.eglCreateWindowSurface(display, configs[0], surface, new int[]{EGL14.EGL_NONE}, 0);
                egl(window != EGL14.EGL_NO_SURFACE, "eglCreateWindowSurface");
                egl(EGL14.eglMakeCurrent(display, window, window, context), "eglMakeCurrent");
            } catch (Throwable failure) {
                try { close(); } catch (Throwable cleanup) { failure.addSuppressed(cleanup); }
                throw failure;
            }
        }
        void draw(int id) {
            GLES20.glViewport(0, 0, WIDTH, HEIGHT);
            GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
            for (int bit = 0; bit < 16; ++bit) {
                int x = (bit % 4) * WIDTH / 4;
                int y = HEIGHT - (bit / 4 + 1) * HEIGHT / 4;
                float value = ((id >>> bit) & 1) == 0 ? 0f : 1f;
                GLES20.glScissor(x, y, WIDTH / 4, HEIGHT / 4);
                GLES20.glClearColor(value, value, value, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            }
            GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
            GLES20.glFinish();
            require(GLES20.glGetError() == GLES20.GL_NO_ERROR, "actual GLES frame rendering failed");
            egl(EGL14.eglSwapBuffers(display, window), "eglSwapBuffers");
        }
        @Override public void close() {
            if (display == EGL14.EGL_NO_DISPLAY) return;
            boolean ok = EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (window != EGL14.EGL_NO_SURFACE) ok &= EGL14.eglDestroySurface(display, window);
            if (context != EGL14.EGL_NO_CONTEXT) ok &= EGL14.eglDestroyContext(display, context);
            ok &= EGL14.eglTerminate(display);
            display = EGL14.EGL_NO_DISPLAY; context = EGL14.EGL_NO_CONTEXT; window = EGL14.EGL_NO_SURFACE;
            require(ok, "EGL producer cleanup failed");
        }
    }

    private static byte[] bytes(ByteBuffer input) {
        require(input != null, "missing real codec configuration");
        ByteBuffer data = input.duplicate();
        byte[] result = new byte[data.remaining()];
        data.get(result);
        return result;
    }
    private static int frameId(Image image) {
        require(image != null && image.getPlanes().length == 3, "decoder did not return real YUV planes");
        Rect crop = image.getCropRect();
        require(crop.width() == WIDTH && crop.height() == HEIGHT, "decoded dimensions changed");
        Image.Plane plane = image.getPlanes()[0];
        ByteBuffer y = plane.getBuffer();
        int id = 0;
        for (int bit = 0; bit < 16; ++bit) {
            int x = crop.left + (bit % 4) * WIDTH / 4 + WIDTH / 8;
            int row = crop.top + (bit / 4) * HEIGHT / 4 + HEIGHT / 8;
            int value = y.get(row * plane.getRowStride() + x * plane.getPixelStride()) & 255;
            require(value < 80 || value > 180, "decoded input identifier is not black/white: " + value);
            if (value > 180) id |= 1 << bit;
        }
        return id;
    }
    private static final class Verified {
        int frames, lastId;
        byte[] sps, pps;
    }
    private static Verified verify(File file, int priorId, int submitted, Verified first) throws Exception {
        require(file.length() > 0 && file.length() < FILE_LIMIT, "completed file size: " + file.length());
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec decoder = null;
        Verified result = new Verified();
        result.lastId = priorId;
        try {
            extractor.setDataSource(file.getPath());
            require(extractor.getTrackCount() == 1, "expected one actual video track");
            MediaFormat format = extractor.getTrackFormat(0);
            require("video/avc".equals(format.getString(MediaFormat.KEY_MIME))
                && format.getInteger(MediaFormat.KEY_WIDTH) == WIDTH
                && format.getInteger(MediaFormat.KEY_HEIGHT) == HEIGHT, "track definition changed");
            require(format.getInteger(MediaFormat.KEY_ROTATION) == 90, "orientation was not retained");
            result.sps = bytes(format.getByteBuffer("csd-0"));
            result.pps = bytes(format.getByteBuffer("csd-1"));
            if (first != null) require(Arrays.equals(first.sps, result.sps) && Arrays.equals(first.pps, result.pps),
                "actual encoder codec configuration changed across files");
            extractor.selectTrack(0);
            require((extractor.getSampleFlags() & MediaExtractor.SAMPLE_FLAG_SYNC) != 0,
                "new segment starts with a dependent frame");
            // Decode source pixels in their physical orientation; verify the
            // independently retained display-orientation metadata above.
            format.setInteger(MediaFormat.KEY_ROTATION, 0);
            decoder = MediaCodec.createDecoderByType("video/avc");
            decoder.configure(format, null, null, 0);
            decoder.start();
            boolean inputEos = false, outputEos = false;
            long inputPts = -1, outputPts = -1;
            int packets = 0;
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20);
            while (!outputEos && System.nanoTime() < deadline) {
                if (!inputEos) {
                    int input = decoder.dequeueInputBuffer(10_000);
                    if (input >= 0) {
                        int size = extractor.readSampleData(decoder.getInputBuffer(input), 0);
                        if (size < 0) {
                            decoder.queueInputBuffer(input, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputEos = true;
                        } else {
                            long pts = extractor.getSampleTime();
                            require(pts >= 0 && pts > inputPts, "container timestamps did not restart/increase");
                            inputPts = pts; ++packets;
                            decoder.queueInputBuffer(input, 0, size, pts, 0);
                            extractor.advance();
                        }
                    }
                }
                int output = decoder.dequeueOutputBuffer(info, 10_000);
                if (output >= 0) {
                    try {
                        if (info.size > 0) {
                            require(info.presentationTimeUs > outputPts, "decoded timestamp order changed");
                            outputPts = info.presentationTimeUs;
                            try (Image image = decoder.getOutputImage(output)) {
                                int id = frameId(image);
                                require(id > result.lastId && id <= submitted,
                                    "decoded frame is not a later submitted input: " + result.lastId + " -> " + id);
                                result.lastId = id; ++result.frames;
                            }
                        }
                        outputEos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    } finally { decoder.releaseOutputBuffer(output, false); }
                }
            }
            require(outputEos && result.frames == packets && packets >= 3,
                "incomplete decoded segment: " + result.frames + "/" + packets);
        } finally {
            if (decoder != null) decoder.release();
            extractor.release();
        }
        say(file.getName() + ": decoded " + result.frames + " actual EGL frame identifiers; key, CSD, orientation and timestamps verified");
        return result;
    }

    private void exercise() {
        HandlerThread thread = new HandlerThread("RecorderRolloverCallbacks");
        thread.start();
        Handler callbacks = new Handler(thread.getLooper());
        MediaRecorder recorder = null;
        Surface surface = null;
        Producer producer = null;
        boolean passed = false;
        AtomicReference<Throwable> callbackFailure = new AtomicReference<>();
        AtomicReference<RandomAccessFile> pendingDescriptor = new AtomicReference<>();
        try {
            File[] files = new File[3];
            for (int i = 0; i < files.length; ++i) {
                files[i] = new File(getFilesDir(), "rollover-" + i + ".mp4");
                require(!files[i].exists() || files[i].delete(), "cannot remove previous test output");
            }
            FutureTask<MediaRecorder> construct = new FutureTask<>(() -> new MediaRecorder(this));
            require(callbacks.post(construct), "callback thread rejected recorder construction");
            recorder = construct.get(10, TimeUnit.SECONDS);
            final MediaRecorder current = recorder;
            argument(() -> current.setNextOutputFile((FileDescriptor) null));
            state(() -> current.setNextOutputFile(files[1]));
            recorder.setVideoSource(MediaRecorder.VideoSource.SURFACE);
            recorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
            recorder.setVideoEncoder(MediaRecorder.VideoEncoder.H264);
            recorder.setVideoSize(WIDTH, HEIGHT);
            recorder.setVideoFrameRate(30);
            recorder.setVideoEncodingBitRate(300000);
            recorder.setOrientationHint(90);
            recorder.setMaxFileSize(FILE_LIMIT);
            recorder.setOutputFile(files[0]);
            state(() -> current.setNextOutputFile(files[1]));
            recorder.prepare();
            surface = recorder.getSurface();
            require(surface != null && surface.isValid(), "missing actual recorder producer");
            final Surface chosen = surface;
            pendingDescriptor.set(new RandomAccessFile(files[1], "rw"));
            CountDownLatch done = new CountDownLatch(1);
            AtomicInteger segment = new AtomicInteger();
            List<Integer> events = new ArrayList<>();
            recorder.setOnInfoListener((r, what, extra) -> {
                try {
                    synchronized (events) { events.add(what); }
                    if (what == MediaRecorder.MEDIA_RECORDER_INFO_MAX_FILESIZE_APPROACHING) {
                        if (segment.get() == 0) {
                            try (RandomAccessFile pending = pendingDescriptor.getAndSet(null)) {
                                require(pending != null, "duplicate approaching notification");
                                r.setNextOutputFile(pending.getFD());
                            }
                        }
                        if (segment.get() == 1) r.setNextOutputFile(files[2]);
                    } else if (what == MediaRecorder.MEDIA_RECORDER_INFO_NEXT_OUTPUT_FILE_STARTED) {
                        require(segment.incrementAndGet() <= 2, "unexpected extra output file");
                        require(r.getSurface() == chosen && chosen.isValid(), "recorder replaced its chosen producer");
                    } else if (what == MediaRecorder.MEDIA_RECORDER_INFO_MAX_FILESIZE_REACHED) {
                        done.countDown();
                    }
                } catch (Throwable failure) { callbackFailure.compareAndSet(null, failure); done.countDown(); }
            });
            recorder.setOnErrorListener((r, what, extra) -> {
                callbackFailure.compareAndSet(null, new AssertionError("recorder error " + what + "/" + extra));
                done.countDown();
            });
            producer = new Producer(surface);
            recorder.start();
            int submitted = -1;
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(45);
            while (done.getCount() != 0 && System.nanoTime() < deadline) {
                ++submitted;
                try { producer.draw(submitted); }
                catch (Throwable afterStop) {
                    // The native producer may retire before the queued 801
                    // reaches Java. Only that confirmed terminal event can
                    // explain a failed final swap; any other failure is fatal.
                    require(done.await(2, TimeUnit.SECONDS), "EGL failed before recorder completion: " + afterStop);
                    synchronized (events) {
                        require(!events.isEmpty() && events.get(events.size() - 1) == 801,
                            "EGL failed without a size-limit completion: " + afterStop);
                    }
                    break;
                }
                Thread.sleep(33);
            }
            require(done.await(2, TimeUnit.SECONDS) && callbackFailure.get() == null,
                "recorder did not complete three files: " + callbackFailure.get());
            CountDownLatch drained = new CountDownLatch(1);
            callbacks.post(drained::countDown);
            require(drained.await(5, TimeUnit.SECONDS), "info callbacks did not drain");
            synchronized (events) {
                require(events.equals(Arrays.asList(802, 803, 802, 803, 802, 801)), "event order: " + events);
            }
            recorder.stop();
            producer.close(); producer = null;
            int prior = -1;
            Verified first = null;
            for (File file : files) {
                Verified result = verify(file, prior, submitted, first);
                if (first == null) first = result;
                prior = result.lastId;
            }
            recorder.release(); recorder = null;
            argument(() -> current.setNextOutputFile((FileDescriptor) null));
            state(() -> current.setNextOutputFile(files[1]));
            passed = true;
        } catch (Throwable failure) {
            say("FAILED " + failure); failure.printStackTrace();
        } finally {
            if (producer != null) try { producer.close(); } catch (Throwable failure) {
                passed = false; say("FAILED EGL cleanup " + failure);
            }
            if (recorder != null) recorder.release();
            if (surface != null) surface.release();
            RandomAccessFile unused = pendingDescriptor.getAndSet(null);
            if (unused != null) try { unused.close(); } catch (Exception failure) {
                passed = false; say("FAILED pending descriptor cleanup " + failure);
            }
            thread.quitSafely();
            try { thread.join(5000); } catch (InterruptedException interrupted) {
                passed = false; Thread.currentThread().interrupt();
            }
            if (thread.isAlive() || callbackFailure.get() != null) passed = false;
            if (passed) say("PASS same EGL producer and recorder across 3 decoded MP4 files; ordered 802/803/801, caller fd closure and lifecycle verified");
            finish();
        }
    }
}
