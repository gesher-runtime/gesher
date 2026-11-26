/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.recordapp;

import android.graphics.ImageFormat;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import java.io.File;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Actual codec/ART/JNI regression gate for Gesher's existing floor-resize path,
 * using the existing explicit camera-fixture recording. Android allows Surface
 * producers to override ImageReader's default dimensions, so the four requested
 * sizes here are not an Android-reference resizing-parity claim.
 */
final class ImageReaderPlaneScalingTest {
    private ImageReaderPlaneScalingTest() {}
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    private static final class Snapshot {
        final int width, height;
        final long timestamp;
        final byte[][] planes;
        Snapshot(Image image) {
            require(image.getFormat() == ImageFormat.YUV_420_888, "actual decoded image format");
            width = image.getWidth(); height = image.getHeight(); timestamp = image.getTimestamp();
            require(width > 0 && height > 0, "actual decoded image dimensions");
            Image.Plane[] input = image.getPlanes();
            require(input.length == 3, "actual decoded image has three planes");
            planes = new byte[3][];
            for (int p = 0; p < 3; ++p) {
                int w = p == 0 ? width : (width + 1) / 2, h = p == 0 ? height : (height + 1) / 2;
                ByteBuffer bytes = input[p].getBuffer().duplicate();
                int start = bytes.position(), row = input[p].getRowStride(), pixel = input[p].getPixelStride();
                require(row > 0 && pixel > 0, "actual image strides are positive");
                require((long) start + (long) (h - 1) * row + (long) (w - 1) * pixel < bytes.limit(),
                        "every logical pixel lies inside its actual plane buffer");
                planes[p] = new byte[w * h];
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        planes[p][y * w + x] = bytes.get(start + y * row + x * pixel);
            }
        }
    }
    private static long remaining(long end) {
        long value = end - System.nanoTime();
        require(value > 0, "real codec/ImageReader scaling exceeded its absolute deadline");
        return value;
    }
    private static Throwable clean(Throwable previous, Runnable action) {
        try { action.run(); } catch (RuntimeException | Error error) {
            if (previous == null) return error;
            previous.addSuppressed(error);
        }
        return previous;
    }
    private static Snapshot decode(File recording, ImageReader reader, Handler handler, long end)
            throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec codec = null;
        Image held = null;
        boolean started = false;
        Throwable failure = null;
        try {
            extractor.setDataSource(recording.getPath());
            int track = -1;
            for (int i = 0; i < extractor.getTrackCount(); ++i) {
                if (extractor.getTrackFormat(i).getString(MediaFormat.KEY_MIME).startsWith("video/")) {
                    track = i; break;
                }
            }
            require(track >= 0, "existing recording has an actual encoded video track");
            extractor.selectTrack(track);
            MediaFormat format = extractor.getTrackFormat(track);
            codec = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME));
            CountDownLatch available = new CountDownLatch(1);
            if (reader != null) reader.setOnImageAvailableListener(r -> available.countDown(), handler);
            codec.configure(format, reader == null ? null : reader.getSurface(), null, 0);
            codec.start(); started = true;
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean inputDone = false;
            for (;;) {
                remaining(end);
                if (!inputDone) {
                    int input = codec.dequeueInputBuffer(Math.min(10000, remaining(end) / 1000));
                    if (input >= 0) {
                        ByteBuffer bytes = codec.getInputBuffer(input);
                        require(bytes != null, "actual decoder input buffer");
                        bytes.clear();
                        int count = extractor.readSampleData(bytes, 0);
                        if (count < 0) {
                            codec.queueInputBuffer(input, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputDone = true;
                        } else {
                            codec.queueInputBuffer(input, 0, count, extractor.getSampleTime(), 0);
                            extractor.advance();
                        }
                    }
                }
                int output = codec.dequeueOutputBuffer(info, Math.min(10000, remaining(end) / 1000));
                if (output < 0) continue;
                if (info.size == 0) {
                    codec.releaseOutputBuffer(output, false);
                    require((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) == 0,
                            "decoder produced a real image before EOS");
                    continue;
                }
                if (reader == null) {
                    held = codec.getOutputImage(output);
                } else {
                    codec.releaseOutputBuffer(output, true);
                    require(available.await(remaining(end), TimeUnit.NANOSECONDS), "actual reader callback arrived");
                    held = reader.acquireNextImage();
                }
                require(held != null, "actual decoded image is available");
                Snapshot result = new Snapshot(held);
                require(result.timestamp == info.presentationTimeUs * 1000, "decoded presentation timestamp");
                if (reader == null) {
                    held.close(); held = null;
                    codec.releaseOutputBuffer(output, false);
                }
                codec.stop(); started = false;
                codec.release(); codec = null;
                if (held != null) {
                    Snapshot retained = new Snapshot(held);
                    require(result.width == retained.width && result.height == retained.height
                                    && result.timestamp == retained.timestamp
                                    && Arrays.deepEquals(result.planes, retained.planes),
                            "acquired reader image owns unchanged planes after codec teardown");
                }
                return result;
            }
        } catch (Exception | Error error) {
            failure = error; throw error;
        } finally {
            Throwable problem = failure;
            final Image image = held;
            final MediaCodec closing = codec;
            if (image != null) problem = clean(problem, image::close);
            if (closing != null && started) problem = clean(problem, closing::stop);
            if (closing != null) problem = clean(problem, closing::release);
            problem = clean(problem, extractor::release);
            if (reader != null) problem = clean(problem, () -> reader.setOnImageAvailableListener(null, null));
            if (failure == null && problem != null) {
                if (problem instanceof Error) throw (Error) problem;
                throw (RuntimeException) problem;
            }
        }
    }
    static void run(File recording) throws Exception {
        require(recording.isFile() && recording.length() > 0, "existing camera-fixture recording is present");
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
        HandlerThread callbacks = new HandlerThread("ReaderPlaneScalingCallbacks");
        callbacks.start();
        Throwable failure = null;
        try {
            Handler handler = new Handler(callbacks.getLooper());
            Snapshot reference = decode(recording, null, handler, end);
            require(reference.width == 320 && reference.height == 240, "actual recorded reference size");
            boolean varies = false;
            for (byte value : reference.planes[0]) varies |= value != reference.planes[0][0];
            require(varies, "decoded reference contains spatial variation for the coordinate oracle");
            int[][] sizes = {{160, 120}, {213, 161}, {641, 481}, {320, 240}};
            for (int[] size : sizes) {
                remaining(end);
                try (ImageReader reader = ImageReader.newInstance(size[0], size[1], ImageFormat.YUV_420_888, 2)) {
                    Snapshot actual = decode(recording, reader, handler, end);
                    require(actual.width == size[0] && actual.height == size[1], "existing Gesher reader target dimensions");
                    require(actual.timestamp == reference.timestamp, "all deliveries preserve the same decoded timestamp");
                    for (int p = 0; p < 3; ++p) {
                        int sw = p == 0 ? reference.width : (reference.width + 1) / 2;
                        int sh = p == 0 ? reference.height : (reference.height + 1) / 2;
                        int dw = p == 0 ? actual.width : (actual.width + 1) / 2;
                        int dh = p == 0 ? actual.height : (actual.height + 1) / 2;
                        for (int y = 0; y < dh; ++y) {
                            int sy = (int) ((long) y * sh / dh);
                            for (int x = 0; x < dw; ++x) {
                                int sx = (int) ((long) x * sw / dw);
                                if (actual.planes[p][y * dw + x] != reference.planes[p][sy * sw + sx])
                                    throw new AssertionError("actual decoded plane " + p + " differs at " + x + "," + y
                                            + " for reader " + dw + "x" + dh);
                            }
                        }
                    }
                }
            }
            remaining(end);
        } catch (Exception | Error error) {
            failure = error; throw error;
        } finally {
            try {
                callbacks.quitSafely();
                callbacks.join(2000);
                require(!callbacks.isAlive(), "owned ImageReader callback thread retired");
            } catch (InterruptedException | RuntimeException | Error error) {
                if (error instanceof InterruptedException) Thread.currentThread().interrupt();
                if (failure == null) throw error;
                failure.addSuppressed(error);
            }
        }
        RecordActivity.say("imagereader plane scaling: pass 4 sizes, exact decoded planes and timestamps, owned teardown");
    }
}
