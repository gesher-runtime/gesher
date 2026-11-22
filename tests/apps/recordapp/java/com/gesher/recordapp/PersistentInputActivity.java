/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.recordapp;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.media.MediaRecorder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.Process;
import android.view.Surface;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

/** One real EGL producer, reused by AVC codec -> recorder -> another AVC codec. */
public final class PersistentInputActivity extends Activity {
    static final int SIZE = 64;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final CountDownLatch connected = new CountDownLatch(1);
    private volatile IBinder remote;
    private boolean bound;
    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            remote = binder; connected.countDown();
        }
        @Override public void onServiceDisconnected(ComponentName name) { remote = null; }
    };

    static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }
    static void say(String text) {
        System.out.println("persistent input: " + text);
        System.out.flush();
    }
    private static void state(Runnable action) {
        try { action.run(); throw new AssertionError("missing IllegalStateException"); }
        catch (IllegalStateException expected) {}
    }
    private static void argument(Runnable action) {
        try { action.run(); throw new AssertionError("missing IllegalArgumentException"); }
        catch (IllegalArgumentException expected) {}
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(this::exercise, "persistent-input-acceptance").start();
    }
    private <T> T ui(java.util.concurrent.Callable<T> work) throws Exception {
        FutureTask<T> task = new FutureTask<>(work);
        main.post(task);
        return task.get(10, TimeUnit.SECONDS);
    }
    private int call(int code, Surface surface, int color) throws Exception {
        IBinder binder = remote;
        require(binder != null, "EGL producer disconnected");
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(PersistentInputService.DESCRIPTOR);
            if (code == PersistentInputService.OPEN) surface.writeToParcel(data, 0);
            if (code == PersistentInputService.DRAW) data.writeInt(color);
            require(binder.transact(code, data, reply, 0), "EGL producer rejected transaction");
            reply.readException();
            return reply.readInt();
        } finally { reply.recycle(); data.recycle(); }
    }

    private interface Producer extends AutoCloseable { void draw(int color) throws Exception; }
    private Producer producer(Surface surface, boolean separate) throws Exception {
        if (!separate) {
            PersistentInputRenderer renderer = new PersistentInputRenderer(surface, SIZE);
            say("local EGL producer pid " + Process.myPid() + " consumer pid " + Process.myPid());
            return new Producer() {
                public void draw(int color) { renderer.draw(color); }
                public void close() { renderer.close(); }
            };
        }
        bound = ui(() -> bindService(new Intent(this, PersistentInputService.class), connection, BIND_AUTO_CREATE));
        require(bound && connected.await(30, TimeUnit.SECONDS), "separate EGL producer bind failed");
        final int pid = call(PersistentInputService.OPEN, surface, 0);
        require(pid != Process.myPid(), "producer must run in a separate process");
        say("separate EGL producer pid " + pid + " consumer pid " + Process.myPid());
        return new Producer() {
            public void draw(int color) throws Exception {
                require(call(PersistentInputService.DRAW, null, color) == pid,
                        "producer identity changed between sessions");
            }
            public void close() throws Exception { call(PersistentInputService.CLOSE, null, 0); }
        };
    }

    private static MediaFormat format() {
        MediaFormat format = MediaFormat.createVideoFormat("video/avc", SIZE, SIZE);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
        format.setInteger(MediaFormat.KEY_BIT_RATE, 400_000);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 0);
        return format;
    }

    private static final class CodecSession implements AutoCloseable {
        final MediaCodec codec;
        final MediaMuxer muxer;
        final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean started, muxing, eos;
        int frames, track = -1;
        long firstPts = -1, lastPts = -1;

        CodecSession(Surface surface, File file, boolean reset) throws Exception {
            codec = MediaCodec.createEncoderByType("video/avc");
            MediaMuxer writer = null;
            try {
                state(() -> codec.setInputSurface(surface));
                argument(() -> codec.setInputSurface(null));
                try (ImageReader ordinary = ImageReader.newInstance(2, 2, PixelFormat.RGBA_8888, 1)) {
                    argument(() -> codec.setInputSurface(ordinary.getSurface()));
                }
                codec.configure(format(), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                codec.setInputSurface(surface);
                codec.setInputSurface(surface); // Same source is valid while configured.
                Surface alternate = MediaCodec.createPersistentInputSurface();
                try {
                    codec.setInputSurface(alternate);
                    require(surface.isValid(), "pre-start replacement abandoned the old persistent producer");
                    codec.setInputSurface(surface);
                } finally { alternate.release(); }
                Surface ordinaryInput = codec.createInputSurface();
                try {
                    require(ordinaryInput != null && ordinaryInput.isValid(), "configured input creation failed");
                    codec.setInputSurface(surface);
                } finally { ordinaryInput.release(); }
                if (reset) {
                    codec.reset();
                    require(surface.isValid(), "codec reset abandoned persistent producer");
                    codec.configure(format(), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                    codec.setInputSurface(surface);
                }
                writer = new MediaMuxer(file.getPath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            } catch (Throwable failure) {
                codec.release();
                if (writer != null) writer.release();
                throw failure;
            }
            muxer = writer;
        }
        void start(Surface surface) {
            codec.start(); started = true;
            state(() -> codec.setInputSurface(surface));
        }
        void drain(int expected, boolean end) {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15);
            while ((frames < expected || (end && !eos)) && System.nanoTime() < deadline) {
                int output = codec.dequeueOutputBuffer(info, 10_000);
                if (output == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    require(!muxing, "unexpected second encoder format");
                    track = muxer.addTrack(codec.getOutputFormat()); muxer.start(); muxing = true;
                } else if (output >= 0) {
                    try {
                        if (info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                            require(muxing, "encoded packet precedes actual output format");
                            require(info.presentationTimeUs >= 0 && info.presentationTimeUs >= lastPts,
                                    "source timestamps are negative or unordered");
                            if (firstPts < 0) firstPts = info.presentationTimeUs;
                            lastPts = info.presentationTimeUs;
                            MediaCodec.BufferInfo sample = new MediaCodec.BufferInfo();
                            sample.set(info.offset, info.size, info.presentationTimeUs - firstPts, info.flags);
                            muxer.writeSampleData(track, codec.getOutputBuffer(output), sample);
                            ++frames;
                        }
                        eos |= (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    } finally { codec.releaseOutputBuffer(output, false); }
                }
            }
            require(frames == expected && (!end || eos), "encoded frame count/EOS did not reach its deadline");
        }
        @Override public void close() {
            try { if (started) codec.stop(); }
            finally {
                try { codec.release(); }
                finally { try { if (muxing) muxer.stop(); } finally { muxer.release(); } }
            }
        }
    }

    private static void configureRecorder(MediaRecorder recorder, Surface surface, File output) {
        recorder.setInputSurface(surface); // The API allows this even before setVideoSource.
        recorder.setVideoSource(MediaRecorder.VideoSource.SURFACE);
        recorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
        recorder.setVideoEncoder(MediaRecorder.VideoEncoder.H264);
        recorder.setVideoSize(SIZE, SIZE);
        recorder.setVideoFrameRate(30);
        recorder.setVideoEncodingBitRate(400_000);
        recorder.setOutputFile(output.getPath());
    }
    private void conflictingOwners(Surface surface, MediaCodec selected) throws Exception {
        MediaCodec other = MediaCodec.createEncoderByType("video/avc");
        Surface busy = MediaCodec.createPersistentInputSurface();
        try {
            other.configure(format(), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            state(() -> other.setInputSurface(surface));
            other.setInputSurface(busy);
            state(() -> selected.setInputSurface(busy));
        } finally { other.release(); busy.release(); }
        MediaRecorder recorder = new MediaRecorder();
        try {
            configureRecorder(recorder, surface, new File(getFilesDir(), "persistent-conflict.mp4"));
            try { recorder.prepare(); throw new AssertionError("second consumer stole the persistent source"); }
            catch (IOException | IllegalStateException expected) {}
            recorder.reset();
        } finally { recorder.release(); }
    }
    private static void encode(CodecSession session, Producer producer, Surface surface, int[] colors) throws Exception {
        producer.draw(Color.WHITE); // Prepared input must discard this frame.
        session.start(surface);
        for (int i = 0; i < colors.length; ++i) {
            producer.draw(colors[i]);
            session.drain(i + 1, false);
        }
        session.codec.signalEndOfInputStream();
        session.drain(colors.length, true);
        producer.draw(Color.WHITE); // Ended input cannot queue another conversion.
    }
    private void record(Surface surface, Producer producer, File file) throws Exception {
        MediaRecorder recorder = new MediaRecorder();
        try {
            try (ImageReader ordinary = ImageReader.newInstance(2, 2, PixelFormat.RGBA_8888, 1)) {
                argument(() -> recorder.setInputSurface(ordinary.getSurface()));
            }
            configureRecorder(recorder, surface, file);
            state(recorder::getSurface);
            recorder.prepare();
            state(recorder::getSurface);
            state(() -> recorder.setInputSurface(surface));
            producer.draw(Color.WHITE);
            recorder.start();
            int[] colors = {Color.RED, Color.GREEN, Color.BLUE};
            for (int i = 0; i < colors.length; ++i) {
                if (i == 1) {
                    recorder.pause(); producer.draw(Color.WHITE); recorder.resume();
                }
                producer.draw(colors[i]); Thread.sleep(20);
                producer.draw(colors[i]); Thread.sleep(20);
            }
            recorder.stop();
            require(surface.isValid(), "recorder stop abandoned the persistent producer");
            // Reset a prepared, never-started recorder: its lease must quiesce too.
            configureRecorder(recorder, surface, new File(file.getParentFile(), "persistent-reset.mp4"));
            recorder.prepare(); recorder.reset();
            require(surface.isValid(), "recorder reset abandoned the persistent producer");
        } finally { recorder.release(); }
    }

    private static int plane(Image image, int index, int x, int y) {
        Image.Plane p = image.getPlanes()[index];
        ByteBuffer bytes = p.getBuffer();
        return bytes.get(bytes.position() + y * p.getRowStride() + x * p.getPixelStride()) & 255;
    }
    private static void pixel(Image image, int color) {
        require(image != null && image.getWidth() == SIZE && image.getHeight() == SIZE,
                "decoded image geometry is not the configured geometry");
        Rect crop = image.getCropRect();
        int x = crop.centerX(), y = crop.centerY();
        int r = Color.red(color), g = Color.green(color), b = Color.blue(color);
        int expectedY = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        int expectedU = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
        int expectedV = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
        require(Math.abs(plane(image, 0, x, y) - expectedY) <= 25
                && Math.abs(plane(image, 1, x / 2, y / 2) - expectedU) <= 25
                && Math.abs(plane(image, 2, x / 2, y / 2) - expectedV) <= 25,
                "decoded pixels differ from the actual EGL producer color");
    }
    private static void decode(File file, int[] colors) throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec decoder = null;
        int frames = 0;
        boolean sentEos = false, receivedEos = false;
        long lastPts = -1;
        try {
            extractor.setDataSource(file.getPath());
            require(extractor.getTrackCount() == 1, "expected a real video-only MP4");
            MediaFormat format = extractor.getTrackFormat(0);
            require("video/avc".equals(format.getString(MediaFormat.KEY_MIME)), "MP4 does not contain AVC");
            extractor.selectTrack(0);
            decoder = MediaCodec.createDecoderByType("video/avc");
            decoder.configure(format, null, null, 0); decoder.start();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20);
            while (!receivedEos && System.nanoTime() < deadline) {
                if (!sentEos) {
                    int input = decoder.dequeueInputBuffer(10_000);
                    if (input >= 0) {
                        int bytes = extractor.readSampleData(decoder.getInputBuffer(input), 0);
                        if (bytes < 0) {
                            decoder.queueInputBuffer(input, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            sentEos = true;
                        } else {
                            decoder.queueInputBuffer(input, 0, bytes, extractor.getSampleTime(), 0);
                            extractor.advance();
                        }
                    }
                }
                int output = decoder.dequeueOutputBuffer(info, 10_000);
                if (output >= 0) {
                    try {
                        if (info.size > 0) {
                            require(frames < colors.length && info.presentationTimeUs >= lastPts,
                                    "unexpected decoded frame or timestamp order");
                            lastPts = info.presentationTimeUs;
                            try (Image image = decoder.getOutputImage(output)) { pixel(image, colors[frames]); }
                            ++frames;
                        }
                        receivedEos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    } finally { decoder.releaseOutputBuffer(output, false); }
                }
            }
            require(receivedEos && frames == colors.length, "decoded frame count/EOS failed");
        } finally {
            if (decoder != null) decoder.release();
            extractor.release();
        }
        say(file.getName() + ": decoded " + frames + " real frames, colors and timestamps verified");
    }

    private void exercise() {
        Surface surface = null;
        Producer producer = null;
        boolean passed = false;
        try {
            surface = MediaCodec.createPersistentInputSurface();
            require(surface != null && surface.isValid(), "persistent factory did not create a valid producer");
            File first = new File(getFilesDir(), "persistent-codec-first.mp4");
            File second = new File(getFilesDir(), "persistent-recorder.mp4");
            File third = new File(getFilesDir(), "persistent-codec-last.mp4");
            int[] forward = {Color.RED, Color.GREEN, Color.BLUE};
            int[] backward = {Color.BLUE, Color.GREEN, Color.RED};
            try (CodecSession session = new CodecSession(surface, first, false)) {
                conflictingOwners(surface, session.codec);
                producer = producer(surface, getIntent().getBooleanExtra("persistent.remote", false));
                encode(session, producer, surface, forward);
            }
            require(surface.isValid(), "codec stop abandoned the persistent source");
            record(surface, producer, second);
            try (CodecSession session = new CodecSession(surface, third, true)) {
                encode(session, producer, surface, backward);
            }
            producer.close(); producer = null;
            decode(first, forward);
            decode(second, new int[]{Color.RED, Color.RED, Color.GREEN, Color.GREEN, Color.BLUE, Color.BLUE});
            decode(third, backward);
            MediaCodec released = MediaCodec.createEncoderByType("video/avc");
            released.release();
            final Surface held = surface;
            state(() -> released.setInputSurface(held));
            surface.release(); surface.release();
            require(!surface.isValid(), "explicit Surface.release left the producer valid");
            MediaCodec fresh = MediaCodec.createEncoderByType("video/avc");
            try {
                fresh.configure(format(), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                argument(() -> fresh.setInputSurface(held));
            } finally { fresh.release(); }
            passed = true;
        } catch (Throwable failure) {
            say("FAILED " + failure); failure.printStackTrace();
        } finally {
            if (producer != null) try { producer.close(); } catch (Exception failure) {
                passed = false; say("FAILED producer close " + failure);
            }
            if (surface != null) surface.release();
            if (bound) try { ui(() -> { unbindService(connection); return null; }); }
            catch (Exception failure) { passed = false; say("FAILED service unbind " + failure); }
            if (passed) say("PASS same EGL producer across codec-recorder-codec, exclusive owners, reset and release, 12 decoded frames");
            main.post(this::finish);
        }
    }
}
