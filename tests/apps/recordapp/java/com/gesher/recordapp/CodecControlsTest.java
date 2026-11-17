package com.gesher.recordapp;

import android.graphics.Canvas;
import android.graphics.Color;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.PersistableBundle;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.Arrays;

import static android.media.MediaCodec.MetricsConstants.*;

final class CodecControlsTest {
    private static final int WIDTH = 320;
    private static final int HEIGHT = 240;
    private static final int FPS = 30;
    private static final int SEGMENT = 90;
    private static final int LOW = 200_000;
    private static final int HIGH = 16_000_000;
    private static final String BITRATE = "android.media.mediacodec.bitrate";

    private CodecControlsTest() {}

    static void run() throws Exception {
        testBuffersAndMetrics();
        testSurfaceAndReuse();
        testAudioMetrics();
        testDecoderMetrics();
        RecordActivity.say("codec controls: live AVC rate and sync controls, ordered output, "
                + "surface retirement, isolated metrics, and lifecycle passed");
    }

    private static MediaFormat videoFormat(int color, int bitrate, int rotation) {
        MediaFormat format = MediaFormat.createVideoFormat("video/avc", WIDTH, HEIGHT);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT, color);
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, FPS);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 10);
        format.setInteger(MediaFormat.KEY_ROTATION, rotation);
        return format;
    }

    private static void testBuffersAndMetrics() throws Exception {
        MediaFormat format = videoFormat(
                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar, LOW, 90);
        MediaCodec codec = MediaCodec.createEncoderByType("video/avc");
        try {
            require(codec.getMetrics().isEmpty(), "fresh codec exposes a previous session");
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            expectIllegalState(() -> codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE));
            PersistableBundle initial = videoMetrics(codec, 90, LOW);
            initial.putInt(BITRATE, -42);
            videoMetrics(codec, 90, LOW);

            codec.start();
            byte[][] input = fixture();
            int[] targets = {LOW, HIGH, LOW};
            long[] bytes = new long[targets.length];
            int submitted = 0, received = 0, headers = 0;
            MediaCodec.BufferInfo output = new MediaCodec.BufferInfo();
            for (int phase = 0; phase < targets.length; phase++) {
                if (phase > 0) setParameters(codec, targets[phase], true);
                int end = (phase + 1) * SEGMENT;
                long deadline = System.nanoTime() + 30_000_000_000L;
                while (received < end && System.nanoTime() < deadline) {
                    if (submitted < end) {
                        int index = codec.dequeueInputBuffer(0);
                        if (index >= 0) {
                            byte[] frame = input[submitted % SEGMENT];
                            ByteBuffer buffer = codec.getInputBuffer(index);
                            buffer.put(frame);
                            codec.queueInputBuffer(index, 0, frame.length, pts(submitted), 0);
                            submitted++;
                        }
                    }
                    int index = codec.dequeueOutputBuffer(output, 5_000);
                    if (index >= 0) {
                        if ((output.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                            headers++;
                        } else {
                            require(output.size > 0 && output.presentationTimeUs == pts(received),
                                    "missing or out-of-order frame " + received);
                            int inPhase = received % SEGMENT;
                            if (phase > 0 && inPhase == 0) {
                                require((output.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0,
                                        "sync request did not reach encoder");
                            }
                            if (inPhase >= SEGMENT / 2) bytes[phase] += output.size;
                            received++;
                        }
                        codec.releaseOutputBuffer(index, false);
                        if (phase == 2 && received == 2 * SEGMENT + 5) {
                            setParameters(codec, Integer.MAX_VALUE, false);
                            setParameters(codec, 0, false);
                        }
                    }
                }
                require(submitted == end && received == end, "encoder did not finish phase " + phase);
                videoMetrics(codec, 90, targets[phase]);
                require(codec.getOutputFormat().getInteger(MediaFormat.KEY_BIT_RATE) == targets[phase],
                        "effective target missing from output format");
            }
            require(headers == 1, "rate change restarted the encoder");
            require(bytes[1] > 1.30 * bytes[0] && bytes[1] > 1.30 * bytes[2],
                    "encoded data did not follow the target changes: " + Arrays.toString(bytes));
            int eos = codec.dequeueInputBuffer(1_000_000);
            require(eos >= 0, "no slot for EOS");
            codec.queueInputBuffer(eos, 0, 0, pts(received), MediaCodec.BUFFER_FLAG_END_OF_STREAM);
            awaitEos(codec);

            codec.stop();
            videoMetrics(codec, 90, LOW);
            expectIllegalState(codec::start);
            codec.reset();
            require(codec.getMetrics().isEmpty(), "reset kept stale codec metrics");
            codec.configure(videoFormat(MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar,
                    HIGH, 0), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            videoMetrics(codec, 0, HIGH);
            RecordActivity.say("codec controls: settled AVC segment bytes " + Arrays.toString(bytes));
        } finally {
            codec.release();
        }
        expectIllegalState(codec::getMetrics);
        expectIllegalState(() -> setParameters(codec, LOW, false));
    }

    private static void testSurfaceAndReuse() throws Exception {
        MediaCodec codec = MediaCodec.createEncoderByType("video/avc");
        try {
            codec.configure(videoFormat(MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface, LOW, 0),
                    null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            Surface input = codec.createInputSurface();
            try {
                codec.start();
                setParameters(codec, HIGH, true);
                Canvas canvas = input.lockCanvas(null);
                canvas.drawColor(Color.BLUE);
                input.unlockCanvasAndPost(canvas);
                codec.signalEndOfInputStream();
                awaitEos(codec);
                videoMetrics(codec, 0, HIGH);
                codec.stop();
                requireRetiredSurface(input);
                videoMetrics(codec, 0, HIGH);
            } finally {
                input.release();
            }
            codec.configure(videoFormat(MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar, LOW, 0),
                    null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            codec.start();
            require(codec.dequeueInputBuffer(1_000_000) >= 0,
                    "former Surface session blocked ByteBuffer input");
            videoMetrics(codec, 0, LOW);
            codec.stop();
        } finally {
            codec.release();
        }
    }

    private static void testAudioMetrics() throws Exception {
        MediaCodec codec = MediaCodec.createEncoderByType("audio/opus");
        try {
            MediaFormat format = MediaFormat.createAudioFormat("audio/opus", 48_000, 1);
            format.setInteger(MediaFormat.KEY_BIT_RATE, 96_000);
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            codec.start();
            setParameters(codec, HIGH, true);
            PersistableBundle metrics = codec.getMetrics();
            require(codec.getName().equals(metrics.getString(CODEC))
                    && "audio/opus".equals(metrics.getString(MIME_TYPE))
                    && MODE_AUDIO.equals(metrics.getString(MODE))
                    && metrics.getInt(ENCODER, -1) == 1
                    && metrics.getInt(SECURE, -1) == 0
                    && metrics.getInt("android.media.mediacodec.sampleRate") == 48_000
                    && metrics.getInt("android.media.mediacodec.channelCount") == 1
                    && !metrics.containsKey(BITRATE)
                    && !metrics.containsKey(MediaCodec.MetricsConstants.WIDTH),
                    "audio metrics contain stale video fields: " + metrics);
            codec.stop();
        } finally {
            codec.release();
        }
    }

    private static PersistableBundle videoMetrics(MediaCodec codec, int rotation, int bitrate) {
        PersistableBundle metrics = codec.getMetrics();
        require(codec.getName().equals(metrics.getString(CODEC))
                && "video/avc".equals(metrics.getString(MIME_TYPE))
                && MODE_VIDEO.equals(metrics.getString(MODE))
                && metrics.getInt(ENCODER, -1) == 1 && metrics.getInt(SECURE, -1) == 0
                && metrics.getInt(MediaCodec.MetricsConstants.WIDTH, -1) == WIDTH
                && metrics.getInt(MediaCodec.MetricsConstants.HEIGHT, -1) == HEIGHT
                && metrics.getInt(ROTATION, -1) == rotation
                && metrics.getInt(BITRATE, -1) == bitrate, "unexpected video metrics: " + metrics);
        return metrics;
    }

    private static void testDecoderMetrics() throws Exception {
        MediaCodec codec = MediaCodec.createDecoderByType("video/avc");
        try {
            MediaFormat format = MediaFormat.createVideoFormat("video/avc", WIDTH, HEIGHT);
            codec.configure(format, null, null, 0);
            codec.start();
            setParameters(codec, HIGH, true);
            PersistableBundle metrics = codec.getMetrics();
            require(codec.getName().equals(metrics.getString(CODEC))
                    && MODE_VIDEO.equals(metrics.getString(MODE))
                    && metrics.getInt(ENCODER, -1) == 0
                    && !metrics.containsKey(BITRATE), "encoder controls leaked into decoder metrics");
            codec.stop();
        } finally {
            codec.release();
        }
    }

    private static long pts(int frame) {
        return frame * 1_000_000L / FPS;
    }

    private static byte[][] fixture() {
        byte[][] frames = new byte[SEGMENT][WIDTH * HEIGHT * 3 / 2];
        for (int step = 0; step < frames.length; step++) {
            byte[] frame = frames[step];
            for (int i = 0; i < frame.length; i++) {
                int value = i % WIDTH * 1234567 + i / WIDTH * 543219 + step * 0x9e3779b1;
                value ^= value >>> 16;
                value *= 0x7feb352d;
                value ^= value >>> 15;
                frame[i] = (byte) (value >>> 24);
            }
        }
        return frames;
    }

    private static void setParameters(MediaCodec codec, int bitrate, boolean sync) {
        Bundle parameters = new Bundle();
        parameters.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrate);
        if (sync) parameters.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
        codec.setParameters(parameters);
    }

    private static void awaitEos(MediaCodec codec) {
        MediaCodec.BufferInfo output = new MediaCodec.BufferInfo();
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (System.nanoTime() < deadline) {
            int index = codec.dequeueOutputBuffer(output, 5_000);
            if (index >= 0) {
                codec.releaseOutputBuffer(index, false);
                if ((output.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) return;
            }
        }
        throw new AssertionError("encoder did not signal EOS");
    }

    private static void expectIllegalState(Runnable call) {
        try {
            call.run();
        } catch (IllegalStateException expected) {
            return;
        }
        throw new AssertionError("operation outside its codec state succeeded");
    }

    private static void requireRetiredSurface(Surface input) throws Exception {
        try {
            Canvas canvas = input.lockCanvas(null);
            input.unlockCanvasAndPost(canvas);
        } catch (IllegalStateException | IllegalArgumentException expected) {
            return;
        }
        throw new AssertionError("stopped encoder left input Surface connected");
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
