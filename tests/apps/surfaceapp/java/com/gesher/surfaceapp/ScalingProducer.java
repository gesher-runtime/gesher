/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.surfaceapp;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.view.Surface;

import java.nio.ByteBuffer;

/** Isolated test producer: real AVC encoding and decoding of three known bars. */
final class ScalingProducer implements AutoCloseable {
    private static final int WIDTH = 192, HEIGHT = 64;
    private static final long WAIT_US = 10_000;
    private final Encoded encoded;
    private Decoder surfaceDecoder, textureDecoder;
    private long timestampUs = 100_000;

    ScalingProducer(Surface surface, Surface texture) throws Exception {
        encoded = encodeBars();
        try {
            surfaceDecoder = new Decoder(surface, encoded);
            textureDecoder = new Decoder(texture, encoded);
        } catch (Throwable failure) {
            close();
            throw failure;
        }
    }

    void render(int mode) throws Exception {
        timestampUs += 100_000;
        surfaceDecoder.render(mode, timestampUs);
        textureDecoder.render(mode, timestampUs);
    }

    void replaceTexture(Surface texture) {
        textureDecoder.codec.setOutputSurface(texture);
        textureDecoder.surface = texture;
    }

    void resetDefaults() {
        surfaceDecoder.reset();
        textureDecoder.reset();
    }

    @Override public void close() {
        Decoder first = surfaceDecoder, second = textureDecoder;
        surfaceDecoder = textureDecoder = null;
        try { if (first != null) first.codec.release(); }
        finally { if (second != null) second.codec.release(); }
    }

    private static final class Encoded {
        byte[] csd0, csd1, sample;
        MediaFormat format() {
            MediaFormat format = MediaFormat.createVideoFormat("video/avc", WIDTH, HEIGHT);
            if (csd0 != null) format.setByteBuffer("csd-0", ByteBuffer.wrap(csd0));
            if (csd1 != null) format.setByteBuffer("csd-1", ByteBuffer.wrap(csd1));
            return format;
        }
    }

    private static byte[] bytes(ByteBuffer value) {
        if (value == null) return null;
        ByteBuffer view = value.duplicate();
        byte[] copy = new byte[view.remaining()];
        view.get(copy);
        return copy;
    }

    private static Encoded encodeBars() throws Exception {
        MediaFormat format = MediaFormat.createVideoFormat("video/avc", WIDTH, HEIGHT);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar);
        format.setInteger(MediaFormat.KEY_BIT_RATE, 800_000);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 0);
        MediaCodec encoder = MediaCodec.createEncoderByType("video/avc");
        try {
            encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            encoder.start();
            Encoded encoded = new Encoded();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            int submitted = 0;
            boolean ended = false;
            long deadline = System.nanoTime() + 30_000_000_000L;
            while (!ended && System.nanoTime() < deadline) {
                if (submitted < 2) {
                    int input = encoder.dequeueInputBuffer(WAIT_US);
                    if (input >= 0) {
                        if (submitted == 0) {
                            ByteBuffer buffer = encoder.getInputBuffer(input);
                            buffer.clear();
                            // Limited-range I420 red, green, blue; 64 source pixels each.
                            int[][] yuv = {{82, 90, 240}, {145, 54, 34}, {41, 240, 110}};
                            for (int plane = 0; plane < 3; plane++) {
                                int w = plane == 0 ? WIDTH : WIDTH / 2;
                                int h = plane == 0 ? HEIGHT : HEIGHT / 2;
                                for (int y = 0; y < h; y++)
                                    for (int x = 0; x < w; x++) buffer.put((byte) yuv[x * 3 / w][plane]);
                            }
                            encoder.queueInputBuffer(input, 0, WIDTH * HEIGHT * 3 / 2, 0, 0);
                        } else {
                            encoder.queueInputBuffer(input, 0, 0, 33_333,
                                    MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        }
                        submitted++;
                    }
                }
                int output = encoder.dequeueOutputBuffer(info, WAIT_US);
                if (output == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat actual = encoder.getOutputFormat();
                    encoded.csd0 = bytes(actual.getByteBuffer("csd-0"));
                    encoded.csd1 = bytes(actual.getByteBuffer("csd-1"));
                } else if (output >= 0) {
                    if (info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                        require((info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0, "encoder omitted key frame");
                        ByteBuffer buffer = encoder.getOutputBuffer(output).duplicate();
                        buffer.position(info.offset).limit(info.offset + info.size);
                        encoded.sample = bytes(buffer);
                    }
                    ended = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    encoder.releaseOutputBuffer(output, false);
                }
            }
            require(ended && encoded.csd0 != null && encoded.sample != null,
                    "encoder did not finish known bar sample");
            return encoded;
        } finally {
            encoder.release();
        }
    }

    private static final class Decoder {
        final MediaCodec codec;
        final Encoded encoded;
        Surface surface;
        Decoder(Surface surface, Encoded encoded) throws Exception {
            this.surface = surface;
            this.encoded = encoded;
            codec = MediaCodec.createDecoderByType("video/avc");
            try {
                // Valid without an output Surface has no effect on a later configure.
                codec.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT_WITH_CROPPING);
                codec.configure(encoded.format(), surface, null, 0);
                codec.start();
            } catch (Throwable failure) {
                codec.release();
                throw failure;
            }
        }

        // One complete stream per render: the IDR sample, then END_OF_STREAM. A
        // decoder need not emit a lone frame before input ends (Android's and
        // Media Foundation's may hold it), so draining is what guarantees output.
        // flush() then readies the started codec for the next stream; unlike
        // configure()/stop()/reset(), it keeps the video scaling mode.
        void render(int mode, long timestampUs) throws Exception {
            if (mode != 0) codec.setVideoScalingMode(mode);
            int queued = 0;
            boolean rendered = false;
            long deadline = System.nanoTime() + 15_000_000_000L;
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            while (System.nanoTime() < deadline) {
                if (queued < 2) {
                    int input = codec.dequeueInputBuffer(WAIT_US);
                    if (input >= 0) {
                        if (queued == 0) {
                            ByteBuffer buffer = codec.getInputBuffer(input);
                            buffer.clear(); buffer.put(encoded.sample);
                            codec.queueInputBuffer(input, 0, encoded.sample.length, timestampUs, 0);
                        } else {
                            codec.queueInputBuffer(input, 0, 0, timestampUs,
                                    MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        }
                        queued++;
                    }
                }
                int output = codec.dequeueOutputBuffer(info, WAIT_US);
                if (output >= 0) {
                    boolean frame = info.size > 0 && !rendered;
                    if (frame) require(info.presentationTimeUs == timestampUs, "decoder changed timestamp");
                    codec.releaseOutputBuffer(output, frame);
                    rendered |= frame;
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                        require(rendered, "decoder reached end of stream without frame for mode " + mode);
                        codec.flush();
                        return;
                    }
                }
            }
            throw new AssertionError("decoder did not finish stream for mode " + mode
                    + (rendered ? " (frame rendered, no end of stream)" : " (no frame)"));
        }

        void reset() {
            codec.reset();
            codec.configure(encoded.format(), surface, null, 0);
            codec.start();
        }
    }

    static void checkErrors() throws Exception {
        MediaCodec codec = MediaCodec.createDecoderByType("video/avc");
        try {
            codec.setVideoScalingMode(1);
            codec.setVideoScalingMode(2);
            for (int mode : new int[]{-1, 0, 3, Integer.MAX_VALUE}) {
                boolean failed = false;
                try { codec.setVideoScalingMode(mode); }
                catch (IllegalArgumentException expected) { failed = true; }
                require(failed, "unknown video mode accepted: " + mode);
            }
        } finally { codec.release(); }
        for (int mode : new int[]{-1, 0, 1, 2, 3}) {
            boolean failed = false;
            try { codec.setVideoScalingMode(mode); }
            catch (IllegalStateException expected) { failed = true; }
            require(failed, "released codec accepted mode: " + mode);
        }
    }

    static void require(boolean okay, String why) {
        if (!okay) throw new AssertionError(why);
    }
}
