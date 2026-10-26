/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.recordapp;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureFailure;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMetadataRetriever;
import android.media.MediaDescription;
import android.media.MediaMetadata;
import android.media.MediaMuxer;
import android.media.MediaRecorder;
import android.media.session.MediaController;
import android.media.session.MediaSession;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Recording the way apps do it (media3, docs/media.md "Recording"). The microphone is
 * GESHER_AUDIO_SOURCE's file (a second, looped from the start of each recording: 750 Hz
 * for 0.4 s, silence, 1250 Hz for 0.4 s, silence); video is four colors of 0.5 s each
 * (red, lime, blue, white), so what comes back says what went in. What the APIs report is
 * printed as "RecordTest ..." lines for tests/windows/tracks/59-media3.ps1.
 */
public class RecordActivity extends Activity {
    static void say(String s) {
        System.out.println("RecordTest " + s);
    }

    static {
        try {
            System.loadLibrary("record_ndk_test");
        } catch (Throwable t) {
            say("ndk: no librecord_ndk_test.so (" + t + ")");
        }
    }

    static native String aaudioInputCallback();
    static native String aaudioInputRead();
    static native String aaudioInputOverruns();
    static native String openslRecorder();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // The steps run here, in onCreate (callbacks come on threads of their own); then the
        // activity finishes, which ends the run in both modes (single process, the device).
        {
            String[] steps = {"session", "devices", "audiorecord", "ndk", "codeclist", "aac", "opus", "h264",
                "h264surface", "muxboth", "recorder", "recordersurface", "recordercamera",
                "imagereader"};
            for (String step : steps) {
                try {
                    switch (step) {
                        case "session": mediaSession(); break;
                        case "devices": devices(); break;
                        case "audiorecord": audioRecord(); break;
                        case "ndk": ndk(); break;
                        case "codeclist": codecList(); break;
                        case "aac": aac(); break;
                        case "opus": opus(); break;
                        case "h264": h264Buffers(); break;
                        case "h264surface": h264Surface(); break;
                        case "muxboth": muxBoth(); break;
                        case "recorder": recorderAudio(); break;
                        case "recordersurface": recorderSurface(); break;
                        case "recordercamera": recorderCamera(); break;
                        case "imagereader": imageReader(); break;
                        default: break;
                    }
                } catch (Throwable t) {
                    say(step + " failed: " + t);
                    t.printStackTrace(System.out);
                }
            }
            say("done");
        }
        finish();
    }

    // --- Signals -----------------------------------------------------------------------

    void mediaSession() throws Exception {
        try {
            Class.forName("android.media.MediaCommunicationManager", false, MediaSession.class.getClassLoader());
            say("session2 manager class present true");
        } catch (ClassNotFoundException e) {
            say("session2 manager class present false (legacy sessions must still work)");
        }
        HandlerThread thread = new HandlerThread("RecordSessionCallbacks");
        thread.start();
        Handler handler = new Handler(thread.getLooper());
        Bundle info = new Bundle();
        info.putString("origin", "recordapp");
        MediaSession session = new MediaSession(this, "record-session", info);
        MediaController controller = new MediaController(this, session.getSessionToken());
        CountDownLatch transport = new CountDownLatch(3), updates = new CountDownLatch(3);
        CountDownLatch released = new CountDownLatch(1);
        AtomicInteger commands = new AtomicInteger();
        MediaController.Callback observer = new MediaController.Callback() {
            @Override public void onMetadataChanged(MediaMetadata m) { updates.countDown(); }
            @Override public void onPlaybackStateChanged(PlaybackState s) { updates.countDown(); }
            @Override public void onQueueChanged(List<MediaSession.QueueItem> q) { updates.countDown(); }
            @Override public void onSessionDestroyed() { released.countDown(); }
        };
        try {
            session.setCallback(new MediaSession.Callback() {
                @Override public void onPlay() { commands.addAndGet(1); transport.countDown(); }
                @Override public void onPause() { commands.addAndGet(2); transport.countDown(); }
                @Override public void onSeekTo(long position) {
                    if (position == 1234) commands.addAndGet(4);
                    transport.countDown();
                }
            }, handler);
            controller.registerCallback(observer, handler);
            session.setMetadata(new MediaMetadata.Builder()
                    .putString(MediaMetadata.METADATA_KEY_TITLE, "Gesher session")
                    .putLong(MediaMetadata.METADATA_KEY_DURATION, 5000).build());
            session.setPlaybackState(new PlaybackState.Builder()
                    .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_SEEK_TO)
                    .setState(PlaybackState.STATE_PAUSED, 100, 1.0f).build());
            session.setQueue(java.util.Arrays.asList(new MediaSession.QueueItem(
                    new MediaDescription.Builder().setMediaId("one").setTitle("Track one").build(), 17)));
            session.setQueueTitle("Gesher queue");
            Bundle extras = new Bundle();
            extras.putString("test", "roundtrip");
            session.setExtras(extras);
            session.setActive(true);
            MediaSessionManager manager = getSystemService(MediaSessionManager.class);
            boolean active = manager.getActiveSessions(null).stream()
                    .anyMatch(c -> c.getSessionToken().equals(session.getSessionToken()));
            boolean state = controller.getMetadata().getString(MediaMetadata.METADATA_KEY_TITLE).equals("Gesher session")
                    && controller.getPlaybackState().getState() == PlaybackState.STATE_PAUSED
                    && controller.getPlaybackState().getPosition() == 100
                    && controller.getQueue().size() == 1 && controller.getQueue().get(0).getQueueId() == 17
                    && "Gesher queue".contentEquals(controller.getQueueTitle())
                    && "roundtrip".equals(controller.getExtras().getString("test"))
                    && "recordapp".equals(controller.getSessionInfo().getString("origin"));
            controller.getTransportControls().play();
            controller.getTransportControls().pause();
            controller.getTransportControls().seekTo(1234);
            boolean called = transport.await(10, TimeUnit.SECONDS) && commands.get() == 7;
            boolean updated = updates.await(10, TimeUnit.SECONDS);
            session.release();
            boolean ended = released.await(10, TimeUnit.SECONDS)
                    && manager.getActiveSessions(null).stream()
                            .noneMatch(c -> c.getSessionToken().equals(session.getSessionToken()));
            if (!active || !state || !called || !updated || !ended) {
                throw new IllegalStateException("active=" + active + " state=" + state + " transport="
                        + called + " updates=" + updated + " released=" + ended);
            }
            say("media session: info, metadata, state, queue, extras, play/pause/seek callbacks, active sessions and release");
        } finally {
            controller.unregisterCallback(observer);
            session.release();
            thread.quitSafely();
            thread.join(10000);
        }
    }

    /** Upward zero crossings (with hysteresis) per second of mono samples in [a, b) s. */
    static double hz(float[] s, int n, int rate, double a, double b) {
        int from = (int) (a * rate), to = Math.min((int) (b * rate), n);
        if (from >= to) return 0;
        long crossings = 0;
        float prev = 0;
        for (int i = from; i < to; i++) {
            if (prev < -0.03f && s[i] >= 0) crossings++;
            if (s[i] < -0.03f || s[i] >= 0) prev = s[i];
        }
        return crossings / ((double) (to - from) / rate);
    }

    static double rms(float[] s, int n, int rate, double a, double b) {
        int from = (int) (a * rate), to = Math.min((int) (b * rate), n);
        double e = 0;
        for (int i = from; i < to; i++) e += s[i] * s[i];
        return to > from ? Math.sqrt(e / (to - from)) : 0;
    }

    static String tone(double hz, int want) {
        return Math.abs(hz - want) <= want * 0.02 ? want + " Hz" : String.format("%.1f Hz", hz);
    }

    /** "750 Hz then 1250 Hz" for a recording that started with the source (margins `m` s). */
    static String tones(float[] s, int n, int rate, double m) {
        return tone(hz(s, n, rate, 0.05 + m, 0.35 + m), 750) + " then "
                + tone(hz(s, n, rate, 0.55 + m, 0.85 + m), 1250);
    }

    static String level(float[] s, int n, int rate) {
        double r = rms(s, n, rate, 0.1, 0.3);
        return Math.abs(r - 0.354) <= 0.354 * 0.15 ? "level ok" : String.format("level %.3f", r);
    }

    // --- Input devices ----------------------------------------------------------------

    void devices() {
        AudioManager am = getSystemService(AudioManager.class);
        AudioDeviceInfo[] in = am.getDevices(AudioManager.GET_DEVICES_INPUTS);
        StringBuilder sb = new StringBuilder();
        for (AudioDeviceInfo d : in) {
            sb.append(" [type ").append(d.getType()).append(", source ").append(d.isSource())
                    .append(", name ").append(d.getProductName()).append("]");
        }
        say("devices: " + in.length + " input(s)" + sb);
    }

    // --- AudioRecord ------------------------------------------------------------------

    void audioRecord() throws Exception {
        int min = AudioRecord.getMinBufferSize(44100, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        AudioRecord r = new AudioRecord(MediaRecorder.AudioSource.MIC, 44100,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, Math.max(min, 8820) * 2);
        int state = r.getState();
        final AtomicInteger periods = new AtomicInteger();
        final CountDownLatch marker = new CountDownLatch(1);
        HandlerThread ht = new HandlerThread("record-events");
        ht.start();
        r.setRecordPositionUpdateListener(new AudioRecord.OnRecordPositionUpdateListener() {
            @Override
            public void onMarkerReached(AudioRecord rec) {
                marker.countDown();
            }

            @Override
            public void onPeriodicNotification(AudioRecord rec) {
                periods.incrementAndGet();
            }
        }, new Handler(ht.getLooper()));
        r.setNotificationMarkerPosition(22050);
        r.setPositionNotificationPeriod(4410);
        r.startRecording();
        int recording = r.getRecordingState();
        short[] pcm = new short[44100];
        int got = 0;
        while (got < pcm.length) {
            int n = r.read(pcm, got, pcm.length - got);
            if (n <= 0) break;
            got += n;
        }
        AudioTimestamp ts = new AudioTimestamp();
        boolean timestamp = r.getTimestamp(ts, AudioTimestamp.TIMEBASE_MONOTONIC) == AudioRecord.SUCCESS
                && ts.framePosition > 0;
        boolean markerReached = marker.await(2, TimeUnit.SECONDS);
        r.stop();
        int routed = r.getRoutedDevice() != null ? r.getRoutedDevice().getType() : -1;
        r.release();
        ht.quitSafely();
        float[] f = new float[got];
        for (int i = 0; i < got; i++) f[i] = pcm[i] / 32768.0f;
        say("audiorecord 16-bit mono 44100: min buffer " + (min > 0 ? "ok" : String.valueOf(min))
                + ", state " + state + ", recording " + recording + ", read " + got + " frames, "
                + tones(f, got, 44100, 0) + ", " + level(f, got, 44100) + ", timestamp " + timestamp
                + ", marker " + markerReached + ", periodic " + (periods.get() >= 8 ? "8+" : periods.get())
                + ", routed to type " + routed);

        // Float, stereo, 48 kHz, with the Builder; a direct ByteBuffer read.
        AudioRecord b = new AudioRecord.Builder()
                .setAudioSource(MediaRecorder.AudioSource.CAMCORDER)
                .setAudioFormat(new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                        .setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_IN_STEREO).build())
                .setBufferSizeInBytes(48000 * 8)
                .build();
        b.startRecording();
        float[] fl = new float[48000 * 2];
        int fgot = 0;
        while (fgot < fl.length) {
            int n = b.read(fl, fgot, fl.length - fgot, AudioRecord.READ_BLOCKING);
            if (n <= 0) break;
            fgot += n;
        }
        b.stop();
        b.release();
        float[] left = new float[fgot / 2];
        for (int i = 0; i < left.length; i++) left[i] = fl[i * 2];
        say("audiorecord float stereo 48000 (builder): channels " + 2 + ", read " + left.length
                + " frames, " + tones(left, left.length, 48000, 0));
    }

    // --- NDK input --------------------------------------------------------------------

    void ndk() {
        say("ndk aaudio input callback: " + aaudioInputCallback());
        say("ndk aaudio input read: " + aaudioInputRead());
        say("ndk aaudio input overruns: " + aaudioInputOverruns());
        say("ndk opensl recorder: " + openslRecorder());
    }

    // --- MediaCodecList ----------------------------------------------------------------

    void codecList() {
        MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        List<String> encoders = new ArrayList<>();
        for (MediaCodecInfo info : list.getCodecInfos()) {
            if (info.isEncoder()) encoders.add(info.getName() + "=" + String.join("+", info.getSupportedTypes()));
        }
        String avc = list.findEncoderForFormat(MediaFormat.createVideoFormat("video/avc", 320, 240));
        String aac = list.findEncoderForFormat(MediaFormat.createAudioFormat("audio/mp4a-latm", 44100, 2));
        String opus = list.findEncoderForFormat(MediaFormat.createAudioFormat("audio/opus", 48000, 2));
        say("codec list encoders (info): " + encoders);
        say("codec list: avc encoder " + avc + ", aac encoder " + aac + ", opus encoder " + opus);
    }

    // --- Encoders and MediaMuxer -------------------------------------------------------

    static short[] tonePcm(int rate, int channels, int hz, double seconds) {
        int n = (int) (rate * seconds);
        short[] s = new short[n * channels];
        for (int i = 0; i < n; i++) {
            short v = (short) (0.5 * 32767 * Math.sin(2 * Math.PI * hz * i / rate));
            for (int c = 0; c < channels; c++) s[i * channels + c] = v;
        }
        return s;
    }

    static class Encoded {
        MediaFormat format;
        final List<byte[]> data = new ArrayList<>();
        final List<MediaCodec.BufferInfo> infos = new ArrayList<>();
        int config, keys;
        String codec;
    }

    /** Raw input through an encoder: PCM for audio; for video, `frames` I420 frames. */
    static Encoded encode(MediaFormat f, short[] pcm, int frames) throws Exception {
        String mime = f.getString(MediaFormat.KEY_MIME);
        MediaCodec codec = MediaCodec.createEncoderByType(mime);
        codec.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        codec.start();
        Encoded e = new Encoded();
        e.codec = codec.getName();
        boolean video = mime.startsWith("video/");
        int rate = video ? 30 : f.getInteger(MediaFormat.KEY_SAMPLE_RATE);
        int channels = video ? 0 : f.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
        int w = video ? f.getInteger(MediaFormat.KEY_WIDTH) : 0, h = video ? f.getInteger(MediaFormat.KEY_HEIGHT) : 0;
        int pos = 0, frame = 0;
        boolean inputDone = false;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long deadline = System.currentTimeMillis() + 60000;
        while (System.currentTimeMillis() < deadline) {
            if (!inputDone) {
                int i = codec.dequeueInputBuffer(10000);
                if (i >= 0) {
                    if (video) {
                        if (frame >= frames) {
                            codec.queueInputBuffer(i, 0, 0, frame * 1000000L / rate, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputDone = true;
                        } else {
                            // Even frames through an Image, odd ones through the ByteBuffer.
                            int[] yuv = COLOR_YUV[frame * 4 / frames];
                            if ((frame & 1) == 0) {
                                Image img = codec.getInputImage(i);
                                fillImage(img, yuv);
                            } else {
                                ByteBuffer in = codec.getInputBuffer(i);
                                fillI420(in, w, h, yuv);
                            }
                            codec.queueInputBuffer(i, 0, w * h * 3 / 2, frame * 1000000L / rate, 0);
                            frame++;
                        }
                    } else {
                        ByteBuffer in = codec.getInputBuffer(i);
                        int samples = Math.min(in.capacity() / 2, Math.min(4096 * channels, pcm.length - pos));
                        if (samples <= 0) {
                            codec.queueInputBuffer(i, 0, 0, (long) (pos / channels) * 1000000L / rate,
                                    MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputDone = true;
                        } else {
                            in.order(ByteOrder.nativeOrder()).asShortBuffer().put(pcm, pos, samples);
                            codec.queueInputBuffer(i, 0, samples * 2, (long) (pos / channels) * 1000000L / rate, 0);
                            pos += samples;
                        }
                    }
                }
            }
            int o = codec.dequeueOutputBuffer(info, 10000);
            if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                e.format = codec.getOutputFormat();
            } else if (o >= 0) {
                ByteBuffer out = codec.getOutputBuffer(o);
                byte[] b = new byte[info.size];
                out.position(info.offset);
                out.get(b);
                MediaCodec.BufferInfo copy = new MediaCodec.BufferInfo();
                copy.set(0, info.size, info.presentationTimeUs, info.flags);
                if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) e.config++;
                if ((info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0) e.keys++;
                if (info.size > 0) {
                    e.data.add(b);
                    e.infos.add(copy);
                }
                codec.releaseOutputBuffer(o, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        codec.stop();
        codec.release();
        return e;
    }

    static void fillI420(ByteBuffer b, int w, int h, int[] yuv) {
        int cw = (w + 1) / 2, ch = (h + 1) / 2;
        b.clear();
        for (int i = 0; i < w * h; i++) b.put((byte) yuv[0]);
        for (int i = 0; i < cw * ch; i++) b.put((byte) yuv[1]);
        for (int i = 0; i < cw * ch; i++) b.put((byte) yuv[2]);
    }

    static void fillImage(Image img, int[] yuv) {
        Image.Plane[] p = img.getPlanes();
        for (int k = 0; k < 3; k++) {
            ByteBuffer b = p[k].getBuffer();
            int w = k == 0 ? img.getWidth() : (img.getWidth() + 1) / 2;
            int h = k == 0 ? img.getHeight() : (img.getHeight() + 1) / 2;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) b.put(y * p[k].getRowStride() + x * p[k].getPixelStride(), (byte) yuv[k]);
            }
        }
    }

    /** Writes encoded tracks into `file` with MediaMuxer (samples interleaved by time). */
    static void mux(File file, int format, Encoded... tracks) throws Exception {
        MediaMuxer m = new MediaMuxer(file.getPath(), format);
        int[] index = new int[tracks.length];
        for (int t = 0; t < tracks.length; t++) index[t] = m.addTrack(tracks[t].format);
        m.start();
        int[] next = new int[tracks.length];
        for (;;) {
            int best = -1;
            for (int t = 0; t < tracks.length; t++) {
                if (next[t] >= tracks[t].data.size()) continue;
                if (best < 0 || tracks[t].infos.get(next[t]).presentationTimeUs
                        < tracks[best].infos.get(next[best]).presentationTimeUs) best = t;
            }
            if (best < 0) break;
            byte[] d = tracks[best].data.get(next[best]);
            MediaCodec.BufferInfo info = tracks[best].infos.get(next[best]);
            next[best]++;
            if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) continue;
            ByteBuffer bb = ByteBuffer.allocateDirect(d.length);
            bb.put(d);
            bb.flip();
            m.writeSampleData(index[best], bb, info);
        }
        m.stop();
        m.release();
    }

    /** A file's tracks, as "mime WxH" / "mime R Hz C ch", and its duration. */
    static String tracks(File file) throws Exception {
        MediaExtractor ex = new MediaExtractor();
        ex.setDataSource(file.getPath());
        List<String> t = new ArrayList<>();
        long duration = 0;
        for (int i = 0; i < ex.getTrackCount(); i++) {
            MediaFormat f = ex.getTrackFormat(i);
            String mime = f.getString(MediaFormat.KEY_MIME);
            if (mime.startsWith("video/")) {
                t.add(mime + " " + f.getInteger(MediaFormat.KEY_WIDTH) + "x" + f.getInteger(MediaFormat.KEY_HEIGHT));
            } else {
                t.add(mime + " " + f.getInteger(MediaFormat.KEY_SAMPLE_RATE) + " Hz "
                        + f.getInteger(MediaFormat.KEY_CHANNEL_COUNT) + " ch");
            }
            if (f.containsKey(MediaFormat.KEY_DURATION)) duration = Math.max(duration, f.getLong(MediaFormat.KEY_DURATION));
        }
        ex.release();
        return t.size() + " track(s) " + t + ", " + (duration / 100000) / 10.0 + " s";
    }

    /** The first audio track of `file` decoded to mono floats; rate in out[0]. */
    static float[] decodeAudio(File file, int[] rateOut) throws Exception {
        MediaExtractor ex = new MediaExtractor();
        ex.setDataSource(file.getPath());
        int a = -1;
        for (int i = 0; i < ex.getTrackCount(); i++) {
            if (ex.getTrackFormat(i).getString(MediaFormat.KEY_MIME).startsWith("audio/")) a = i;
        }
        if (a < 0) {
            ex.release();
            return new float[0];
        }
        MediaFormat f = ex.getTrackFormat(a);
        ex.selectTrack(a);
        MediaCodec codec = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
        codec.configure(f, null, null, 0);
        codec.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean inputDone = false;
        int channels = 1, rate = 0;
        float[] out = new float[48000 * 10];
        int n = 0;
        long deadline = System.currentTimeMillis() + 20000;
        while (System.currentTimeMillis() < deadline) {
            if (!inputDone) {
                int i = codec.dequeueInputBuffer(10000);
                if (i >= 0) {
                    int s = ex.readSampleData(codec.getInputBuffer(i), 0);
                    if (s < 0) {
                        codec.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        codec.queueInputBuffer(i, 0, s, ex.getSampleTime(), 0);
                        ex.advance();
                    }
                }
            }
            int o = codec.dequeueOutputBuffer(info, 10000);
            if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat of = codec.getOutputFormat();
                channels = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                rate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE);
            } else if (o >= 0) {
                ByteBuffer b = codec.getOutputBuffer(o);
                if (b != null) {
                    b.order(ByteOrder.LITTLE_ENDIAN);
                    int frames = info.size / (2 * channels);
                    for (int k = 0; k < frames && n < out.length; k++) {
                        out[n++] = b.getShort(info.offset + k * 2 * channels) / 32768.0f;
                    }
                }
                codec.releaseOutputBuffer(o, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        codec.stop();
        codec.release();
        ex.release();
        rateOut[0] = rate;
        return java.util.Arrays.copyOf(out, n);
    }

    // The colors in BT.601 limited-range YUV, by quarter.
    static final String[] COLOR_NAMES = {"red", "lime", "blue", "white"};
    static final int[][] COLOR_YUV = {{82, 90, 240}, {145, 54, 34}, {41, 240, 110}, {235, 128, 128}};
    static final int[] COLOR_RGB = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};

    static String colorOf(int y, int u, int v) {
        for (int i = 0; i < COLOR_YUV.length; i++) {
            if (Math.abs(y - COLOR_YUV[i][0]) <= 12 && Math.abs(u - COLOR_YUV[i][1]) <= 12
                    && Math.abs(v - COLOR_YUV[i][2]) <= 12) {
                return COLOR_NAMES[i];
            }
        }
        return "(" + y + "," + u + "," + v + ")";
    }

    /** What decoding a file's video track saw: frames, the colors in order (runs), a pixel. */
    static class Video {
        int frames;
        final List<String> runs = new ArrayList<>();
        int sampleY = -1, sampleU = -1, sampleV = -1;
        long lastPts = -1;
        boolean ordered = true;

        void color(String c) {
            if (runs.isEmpty() || !runs.get(runs.size() - 1).equals(c)) runs.add(c);
        }
    }

    /** Decodes `file`'s video track (to ByteBuffers, or into `surface`). */
    static Video decodeVideo(File file, Surface surface, int sx, int sy) throws Exception {
        MediaExtractor ex = new MediaExtractor();
        ex.setDataSource(file.getPath());
        int v = -1;
        for (int i = 0; i < ex.getTrackCount(); i++) {
            if (ex.getTrackFormat(i).getString(MediaFormat.KEY_MIME).startsWith("video/")) v = i;
        }
        Video r = new Video();
        if (v < 0) {
            ex.release();
            return r;
        }
        MediaFormat f = ex.getTrackFormat(v);
        ex.selectTrack(v);
        MediaCodec codec = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
        codec.configure(f, surface, null, 0);
        codec.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean inputDone = false;
        long deadline = System.currentTimeMillis() + 30000;
        while (System.currentTimeMillis() < deadline) {
            if (!inputDone) {
                int i = codec.dequeueInputBuffer(10000);
                if (i >= 0) {
                    int s = ex.readSampleData(codec.getInputBuffer(i), 0);
                    if (s < 0) {
                        codec.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        codec.queueInputBuffer(i, 0, s, ex.getSampleTime(), 0);
                        ex.advance();
                    }
                }
            }
            int o = codec.dequeueOutputBuffer(info, 10000);
            if (o >= 0) {
                if (info.size > 0) {
                    if (info.presentationTimeUs < r.lastPts) r.ordered = false;
                    r.lastPts = info.presentationTimeUs;
                    if (surface == null) {
                        MediaFormat of = codec.getOutputFormat(o);
                        int w = of.getInteger(MediaFormat.KEY_WIDTH), h = of.getInteger(MediaFormat.KEY_HEIGHT);
                        ByteBuffer b = codec.getOutputBuffer(o);
                        int base = b.position(), cw = (w + 1) / 2, ch = (h + 1) / 2;
                        int x = w / 2, y = h / 2;
                        r.color(colorOf(b.get(base + y * w + x) & 0xff,
                                b.get(base + w * h + (y / 2) * cw + x / 2) & 0xff,
                                b.get(base + w * h + cw * ch + (y / 2) * cw + x / 2) & 0xff));
                        if (r.frames == 10 && sx >= 0) {
                            r.sampleY = b.get(base + sy * w + sx) & 0xff;
                            r.sampleU = b.get(base + w * h + (sy / 2) * cw + sx / 2) & 0xff;
                            r.sampleV = b.get(base + w * h + cw * ch + (sy / 2) * cw + sx / 2) & 0xff;
                        }
                    }
                    r.frames++;
                }
                codec.releaseOutputBuffer(o, surface != null && info.size > 0);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        codec.stop();
        codec.release();
        ex.release();
        return r;
    }

    void aac() throws Exception {
        MediaFormat f = MediaFormat.createAudioFormat("audio/mp4a-latm", 44100, 2);
        f.setInteger(MediaFormat.KEY_BIT_RATE, 128000);
        f.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC);
        Encoded e = encode(f, tonePcm(44100, 2, 1000, 1.0), 0);
        ByteBuffer csd = e.format != null ? e.format.getByteBuffer("csd-0") : null;
        File file = new File(getFilesDir(), "aac.mp4");
        mux(file, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4, e);
        int[] rate = new int[1];
        float[] pcm = decodeAudio(file, rate);
        say("encoder aac: " + e.codec + ", csd-0 " + (csd != null ? csd.remaining() + " bytes" : "none")
                + ", config buffers " + e.config + ", " + (e.data.size() >= 40 ? "40+" : String.valueOf(e.data.size()))
                + " packets; MPEG-4 by MediaMuxer: " + tracks(file) + "; decoded "
                + tone(hz(pcm, pcm.length, Math.max(rate[0], 1), 0.2, 0.8), 1000));
    }

    void opus() throws Exception {
        MediaFormat f = MediaFormat.createAudioFormat("audio/opus", 48000, 1);
        f.setInteger(MediaFormat.KEY_BIT_RATE, 64000);
        Encoded e = encode(f, tonePcm(48000, 1, 1000, 1.0), 0);
        ByteBuffer csd = e.format != null ? e.format.getByteBuffer("csd-0") : null;
        boolean head = csd != null && csd.remaining() >= 19 && csd.get(0) == 'O' && csd.get(4) == 'H';
        File file = new File(getFilesDir(), "opus.webm");
        mux(file, MediaMuxer.OutputFormat.MUXER_OUTPUT_WEBM, e);
        int[] rate = new int[1];
        float[] pcm = decodeAudio(file, rate);
        say("encoder opus: " + e.codec + ", OpusHead " + head + ", "
                + (e.data.size() >= 40 ? "40+" : String.valueOf(e.data.size())) + " packets; WebM by MediaMuxer: "
                + tracks(file) + "; decoded " + tone(hz(pcm, pcm.length, Math.max(rate[0], 1), 0.2, 0.8), 1000));
    }

    void h264Buffers() throws Exception {
        MediaFormat f = MediaFormat.createVideoFormat("video/avc", 320, 240);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        f.setInteger(MediaFormat.KEY_BIT_RATE, 1000000);
        f.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
        Encoded e = encode(f, null, 60);
        ByteBuffer sps = e.format != null ? e.format.getByteBuffer("csd-0") : null;
        ByteBuffer pps = e.format != null ? e.format.getByteBuffer("csd-1") : null;
        boolean spsOk = sps != null && sps.remaining() > 4 && (sps.get(4) & 0x1f) == 7;
        boolean ppsOk = pps != null && pps.remaining() > 4 && (pps.get(4) & 0x1f) == 8;
        File file = new File(getFilesDir(), "h264.mp4");
        mux(file, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4, e);
        Video v = decodeVideo(file, null, -1, -1);
        say("encoder h264 (buffers and images): " + e.codec + ", csd-0 SPS " + spsOk + ", csd-1 PPS " + ppsOk
                + ", config buffers " + e.config + ", "
                + e.infos.stream().filter(i -> (i.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0).count()
                + " frames, key frames "
                + (e.keys >= 2 ? "2+" : String.valueOf(e.keys)) + "; MPEG-4 by MediaMuxer: " + tracks(file)
                + "; decoded " + v.frames + " frames, ordered " + v.ordered + ", colors " + v.runs);
    }

    void h264Surface() throws Exception {
        MediaFormat f = MediaFormat.createVideoFormat("video/avc", 320, 240);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        f.setInteger(MediaFormat.KEY_BIT_RATE, 1000000);
        f.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
        MediaCodec codec = MediaCodec.createEncoderByType("video/avc");
        codec.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        Surface input = codec.createInputSurface();
        codec.start();
        Encoded e = new Encoded();
        e.codec = codec.getName();
        Thread drawer = new Thread(() -> {
            try {
                draw(input, 60);
            } catch (Throwable t) {
                say("h264 surface draw: " + t);
            }
            codec.signalEndOfInputStream();
        });
        drawer.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long deadline = System.currentTimeMillis() + 30000;
        while (System.currentTimeMillis() < deadline) {
            int o = codec.dequeueOutputBuffer(info, 10000);
            if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                e.format = codec.getOutputFormat();
            } else if (o >= 0) {
                ByteBuffer out = codec.getOutputBuffer(o);
                byte[] b = new byte[info.size];
                out.position(info.offset);
                out.get(b);
                MediaCodec.BufferInfo copy = new MediaCodec.BufferInfo();
                copy.set(0, info.size, info.presentationTimeUs, info.flags);
                if ((info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0) e.keys++;
                if (info.size > 0) {
                    e.data.add(b);
                    e.infos.add(copy);
                }
                codec.releaseOutputBuffer(o, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        drawer.join(5000);
        codec.stop();
        codec.release();
        input.release();
        File file = new File(getFilesDir(), "h264_surface.mp4");
        mux(file, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4, e);
        Video v = decodeVideo(file, null, -1, -1);
        say("encoder h264 (input surface): " + e.codec + ", " + (e.data.size() >= 50 ? "50+" : String.valueOf(e.data.size()))
                + " frames; MPEG-4 by MediaMuxer: " + tracks(file) + "; decoded "
                + (v.frames >= 50 ? "50+" : String.valueOf(v.frames)) + " frames, ordered " + v.ordered
                + ", colors " + v.runs);
    }

    /** Draws `frames` frames of the four colors into `s` at 30 frames a second (Canvas). */
    static void draw(Surface s, int frames) throws Exception {
        long start = System.nanoTime();
        for (int i = 0; i < frames; i++) {
            Canvas c = s.lockCanvas(null);
            c.drawColor(COLOR_RGB[i * 4 / frames]);
            s.unlockCanvasAndPost(c);
            long next = start + (i + 1) * 1000000000L / 30;
            long wait = (next - System.nanoTime()) / 1000000;
            if (wait > 0) Thread.sleep(wait);
        }
    }

    void muxBoth() throws Exception {
        MediaFormat af = MediaFormat.createAudioFormat("audio/mp4a-latm", 48000, 1);
        af.setInteger(MediaFormat.KEY_BIT_RATE, 96000);
        Encoded a = encode(af, tonePcm(48000, 1, 1000, 2.0), 0);
        MediaFormat vf = MediaFormat.createVideoFormat("video/avc", 320, 240);
        vf.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar);
        vf.setInteger(MediaFormat.KEY_BIT_RATE, 1000000);
        vf.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
        vf.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
        Encoded v = encode(vf, null, 60);
        File file = new File(getFilesDir(), "both.mp4");
        mux(file, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4, v, a);
        MediaMetadataRetriever r = new MediaMetadataRetriever();
        r.setDataSource(file.getPath());
        String frame = describe(r.getFrameAtTime(1250000, MediaMetadataRetriever.OPTION_CLOSEST));
        String dur = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION);
        r.release();
        int[] rate = new int[1];
        float[] pcm = decodeAudio(file, rate);
        say("muxer mp4 (video and audio): " + tracks(file) + "; retriever duration "
                + (dur != null && Math.abs(Integer.parseInt(dur) - 2000) <= 100 ? "ok" : dur)
                + ", frame at 1.25 s " + frame + "; audio " + tone(hz(pcm, pcm.length, Math.max(rate[0], 1), 0.2, 1.8), 1000));
    }

    static String describe(android.graphics.Bitmap b) {
        if (b == null) return "null";
        int argb = b.getPixel(b.getWidth() / 2, b.getHeight() / 2);
        int r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, bl = argb & 0xff;
        String name = r > 200 && g < 60 && bl < 60 ? "red" : r < 60 && g > 200 && bl < 60 ? "lime"
                : r < 60 && g < 60 && bl > 200 ? "blue" : r > 200 && g > 200 && bl > 200 ? "white"
                : String.format("#%06x", argb & 0xffffff);
        return b.getWidth() + "x" + b.getHeight() + " " + name;
    }

    // --- MediaRecorder -----------------------------------------------------------------

    /** Records `ms` of the microphone into `file`. */
    String recordAudio(File file, int format, int encoder, int rate, int ms) throws Exception {
        MediaRecorder r = new MediaRecorder(this);
        r.setAudioSource(MediaRecorder.AudioSource.MIC);
        r.setOutputFormat(format);
        r.setAudioEncoder(encoder);
        if (rate > 0) r.setAudioSamplingRate(rate);
        r.setAudioChannels(1);
        r.setAudioEncodingBitRate(96000);
        r.setOutputFile(file.getPath());
        r.prepare();
        r.start();
        int amplitude = 0;
        long end = System.currentTimeMillis() + ms;
        while (System.currentTimeMillis() < end) {
            Thread.sleep(100);
            amplitude = Math.max(amplitude, r.getMaxAmplitude());
        }
        r.stop();
        r.release();
        return "max amplitude " + (amplitude > 10000 ? "loud" : String.valueOf(amplitude));
    }

    void recorderAudio() throws Exception {
        File mp4 = new File(getFilesDir(), "rec_aac.m4a");
        String amp = recordAudio(mp4, MediaRecorder.OutputFormat.MPEG_4, MediaRecorder.AudioEncoder.AAC, 44100, 1500);
        int[] rate = new int[1];
        float[] pcm = decodeAudio(mp4, rate);
        say("recorder aac mpeg4: " + amp + ", " + tracks(mp4) + ", decoded " + tones(pcm, pcm.length, Math.max(rate[0], 1), 0.05));

        File ogg = new File(getFilesDir(), "rec_opus.ogg");
        amp = recordAudio(ogg, MediaRecorder.OutputFormat.OGG, MediaRecorder.AudioEncoder.OPUS, 48000, 1500);
        pcm = decodeAudio(ogg, rate);
        say("recorder opus ogg: " + amp + ", " + tracks(ogg) + ", decoded " + tones(pcm, pcm.length, Math.max(rate[0], 1), 0.05));

        File webm = new File(getFilesDir(), "rec_opus.webm");
        amp = recordAudio(webm, MediaRecorder.OutputFormat.WEBM, MediaRecorder.AudioEncoder.OPUS, 48000, 1500);
        pcm = decodeAudio(webm, rate);
        say("recorder opus webm: " + amp + ", " + tracks(webm) + ", decoded " + tones(pcm, pcm.length, Math.max(rate[0], 1), 0.05));

        File adts = new File(getFilesDir(), "rec.aac");
        amp = recordAudio(adts, MediaRecorder.OutputFormat.AAC_ADTS, MediaRecorder.AudioEncoder.AAC, 48000, 1500);
        pcm = decodeAudio(adts, rate);
        say("recorder aac adts: " + amp + ", " + tracks(adts) + ", decoded " + tones(pcm, pcm.length, Math.max(rate[0], 1), 0.05));
    }

    /** MediaRecorder: video from its Surface (Canvas, the four colors over 2 s) with the microphone. */
    void recorderSurface() throws Exception {
        File file = new File(getFilesDir(), "rec_surface.mp4");
        MediaRecorder r = new MediaRecorder(this);
        r.setAudioSource(MediaRecorder.AudioSource.MIC);
        r.setVideoSource(MediaRecorder.VideoSource.SURFACE);
        r.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
        r.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
        r.setVideoEncoder(MediaRecorder.VideoEncoder.H264);
        r.setVideoSize(320, 240);
        r.setVideoFrameRate(30);
        r.setVideoEncodingBitRate(1000000);
        r.setAudioSamplingRate(44100);
        r.setOutputFile(file.getPath());
        r.prepare();
        Surface s = r.getSurface();
        boolean same = s == r.getSurface();
        r.start();
        draw(s, 60);
        r.stop();
        r.release();
        Video v = decodeVideo(file, null, -1, -1);
        int[] rate = new int[1];
        float[] pcm = decodeAudio(file, rate);
        MediaMetadataRetriever mr = new MediaMetadataRetriever();
        mr.setDataSource(file.getPath());
        String frame = describe(mr.getFrameAtTime(1250000, MediaMetadataRetriever.OPTION_CLOSEST));
        mr.release();
        say("recorder surface: getSurface the same " + same + ", " + tracks(file) + "; video "
                + (v.frames >= 45 ? "45+" : String.valueOf(v.frames)) + " frames, colors " + v.runs
                + ", frame at 1.25 s " + frame + "; audio " + tones(pcm, pcm.length, Math.max(rate[0], 1), 0.05));
    }

    /** MediaRecorder: video from the camera (Camera2, a repeating request into getSurface()). */
    void recorderCamera() throws Exception {
        CameraManager cm = getSystemService(CameraManager.class);
        String[] ids = cm.getCameraIdList();
        if (ids.length == 0) {
            say("recorder camera: no camera");
            return;
        }
        File file = new File(getFilesDir(), "rec_camera.mp4");
        MediaRecorder r = new MediaRecorder(this);
        r.setVideoSource(MediaRecorder.VideoSource.SURFACE);
        r.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
        r.setVideoEncoder(MediaRecorder.VideoEncoder.H264);
        r.setVideoSize(320, 240);
        r.setVideoFrameRate(30);
        r.setOutputFile(file.getPath());
        r.prepare();
        Surface surface = r.getSurface();
        HandlerThread ht = new HandlerThread("camera");
        ht.start();
        Handler h = new Handler(ht.getLooper());
        final CameraDevice[] device = new CameraDevice[1];
        final CameraCaptureSession[] session = new CameraCaptureSession[1];
        CountDownLatch opened = new CountDownLatch(1), configured = new CountDownLatch(1);
        CountDownLatch closed = new CountDownLatch(1);
        AtomicInteger closeCallbacks = new AtomicInteger();
        String id = ids[ids.length - 1];
        cm.openCamera(id, new CameraDevice.StateCallback() {
            @Override
            public void onOpened(CameraDevice d) {
                device[0] = d;
                opened.countDown();
            }

            @Override
            public void onDisconnected(CameraDevice d) {
                d.close();
            }

            @Override
            public void onError(CameraDevice d, int error) {
                say("recorder camera: open error " + error);
                opened.countDown();
            }

            @Override
            public void onClosed(CameraDevice d) {
                closeCallbacks.incrementAndGet();
                closed.countDown();
            }
        }, h);
        if (!opened.await(10, TimeUnit.SECONDS) || device[0] == null) {
            say("recorder camera: not opened");
            r.release();
            ht.quitSafely();
            return;
        }
        device[0].createCaptureSession(new SessionConfiguration(SessionConfiguration.SESSION_REGULAR,
                List.of(new OutputConfiguration(surface)), h::post, new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession s) {
                        session[0] = s;
                        configured.countDown();
                    }

                    @Override
                    public void onConfigureFailed(CameraCaptureSession s) {
                        say("recorder camera: configure failed");
                        configured.countDown();
                    }
                }));
        configured.await(10, TimeUnit.SECONDS);
        if (session[0] == null) {
            device[0].close();
            r.release();
            ht.quitSafely();
            return;
        }
        CaptureRequest.Builder b = device[0].createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
        b.addTarget(surface);
        AtomicInteger captureStarted = new AtomicInteger(), captureCompleted = new AtomicInteger();
        AtomicInteger captureFailed = new AtomicInteger(), buffersLost = new AtomicInteger();
        AtomicLong firstSourceNs = new AtomicLong(), lastSourceNs = new AtomicLong();
        AtomicLong firstFrame = new AtomicLong(-1), lastFrame = new AtomicLong(-1);
        r.start();
        long collectStartNs = System.nanoTime();
        int requestId = session[0].setRepeatingRequest(b.build(), new CameraCaptureSession.CaptureCallback() {
            @Override
            public void onCaptureStarted(CameraCaptureSession s, CaptureRequest request, long timestamp, long frameNumber) {
                captureStarted.incrementAndGet();
                firstSourceNs.compareAndSet(0, timestamp);
                lastSourceNs.set(timestamp);
                firstFrame.compareAndSet(-1, frameNumber);
                lastFrame.set(frameNumber);
            }

            @Override
            public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest request, TotalCaptureResult result) {
                captureCompleted.incrementAndGet();
            }

            @Override
            public void onCaptureFailed(CameraCaptureSession s, CaptureRequest request, CaptureFailure failure) {
                captureFailed.incrementAndGet();
            }

            @Override
            public void onCaptureBufferLost(CameraCaptureSession s, CaptureRequest request, Surface target, long frameNumber) {
                buffersLost.incrementAndGet();
            }
        }, h);
        Thread.sleep(2000);
        long collectStopNs = System.nanoTime();
        session[0].stopRepeating();
        session[0].close();
        long drainStartNs = System.nanoTime();
        r.stop();
        long drainStopNs = System.nanoTime();
        r.release();
        device[0].close();
        boolean closedBeforeQuit = closed.await(5, TimeUnit.SECONDS) && ht.isAlive()
                && closeCallbacks.get() == 1;
        ht.quitSafely();
        ht.join(2000);
        closedBeforeQuit &= closeCallbacks.get() == 1;
        say("recorder camera close: onClosed exactly once before callback thread quit "
                + closedBeforeQuit + ", callback thread stopped " + !ht.isAlive());
        say("recorder camera trace: request " + requestId + ", started " + captureStarted.get()
                + ", completed " + captureCompleted.get() + ", failed " + captureFailed.get()
                + ", buffers lost " + buffersLost.get() + ", source ns " + firstSourceNs.get() + ".." + lastSourceNs.get()
                + ", source frame numbers " + firstFrame.get() + ".." + lastFrame.get()
                + ", collection ns " + collectStartNs + ".." + collectStopNs
                + ", stop/drain ns " + drainStartNs + ".." + drainStopNs);
        // The test camera's picture: Y = x + y (at its own size: the recorder's 320x240 takes
        // every other pixel of 640x480, so Y = 2 (x + y)), gray chroma, away from the square.
        Video v = decodeVideo(file, null, 60, 40);
        boolean pattern = Math.abs(v.sampleY - 200) <= 16 && Math.abs(v.sampleU - 128) <= 12
                && Math.abs(v.sampleV - 128) <= 12;
        say("recorder camera " + id + ": " + tracks(file) + "; video " + (v.frames >= 30 ? "30+" : String.valueOf(v.frames))
                + " frames, ordered " + v.ordered + ", the test picture " + pattern + " (at 60,40: Y " + v.sampleY
                + ", U " + v.sampleU + ", V " + v.sampleV + ")");
    }

    // --- MediaCodec into an ImageReader ------------------------------------------------

    void imageReader() throws Exception {
        File file = new File(getFilesDir(), "h264.mp4");
        if (!file.exists()) {
            say("imagereader: no h264.mp4");
            return;
        }
        HandlerThread ht = new HandlerThread("images");
        ht.start();
        ImageReader reader = ImageReader.newInstance(320, 240, ImageFormat.YUV_420_888, 4);
        final Video seen = new Video();
        final String[] format = new String[1];
        reader.setOnImageAvailableListener(rd -> {
            Image img = rd.acquireNextImage();
            if (img == null) return;
            Image.Plane[] p = img.getPlanes();
            int x = img.getWidth() / 2, y = img.getHeight() / 2;
            synchronized (seen) {
                format[0] = img.getFormat() + " " + img.getWidth() + "x" + img.getHeight() + " " + p.length + " planes";
                seen.color(colorOf(p[0].getBuffer().get(y * p[0].getRowStride() + x * p[0].getPixelStride()) & 0xff,
                        p[1].getBuffer().get((y / 2) * p[1].getRowStride() + (x / 2) * p[1].getPixelStride()) & 0xff,
                        p[2].getBuffer().get((y / 2) * p[2].getRowStride() + (x / 2) * p[2].getPixelStride()) & 0xff));
                seen.frames++;
            }
            img.close();
        }, new Handler(ht.getLooper()));
        Video v = decodeVideoSlowly(file, reader.getSurface());
        Thread.sleep(500);
        reader.close();
        ht.quitSafely();
        synchronized (seen) {
            say("imagereader: decoder rendered " + v.frames + " frames into the reader's surface; images "
                    + (seen.frames >= 40 ? "40+" : String.valueOf(seen.frames)) + " (" + format[0] + "), colors " + seen.runs);
        }
    }

    /** decodeVideo into a Surface, a frame each 15 ms (the reader's listener keeps up). */
    static Video decodeVideoSlowly(File file, Surface surface) throws Exception {
        MediaExtractor ex = new MediaExtractor();
        ex.setDataSource(file.getPath());
        ex.selectTrack(0);
        MediaFormat f = ex.getTrackFormat(0);
        MediaCodec codec = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
        codec.configure(f, surface, null, 0);
        codec.start();
        Video r = new Video();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean inputDone = false;
        long deadline = System.currentTimeMillis() + 30000;
        while (System.currentTimeMillis() < deadline) {
            if (!inputDone) {
                int i = codec.dequeueInputBuffer(10000);
                if (i >= 0) {
                    int s = ex.readSampleData(codec.getInputBuffer(i), 0);
                    if (s < 0) {
                        codec.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        codec.queueInputBuffer(i, 0, s, ex.getSampleTime(), 0);
                        ex.advance();
                    }
                }
            }
            int o = codec.dequeueOutputBuffer(info, 10000);
            if (o >= 0) {
                boolean render = info.size > 0;
                if (render) r.frames++;
                codec.releaseOutputBuffer(o, render);
                if (render) Thread.sleep(15);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        codec.stop();
        codec.release();
        ex.release();
        return r;
    }
}
