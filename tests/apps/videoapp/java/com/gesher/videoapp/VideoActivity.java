/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.videoapp;

import android.app.Activity;
import android.content.res.AssetFileDescriptor;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Video the way apps decode it (docs/media.md "Video"): the clips are 2 s at 320x240 and
 * 30 frames a second, red, lime, blue and white for 0.5 s each, so a frame's color says
 * which one it is. What the APIs report is printed as "VideoTest ..." lines for
 * tests/windows/tracks/38-media.ps1.
 */
public class VideoActivity extends Activity {
    static void say(String s) {
        System.out.println("VideoTest " + s);
    }

    // The clips' colors in BT.601 limited-range YUV (Y, U, V), by quarter.
    static final String[] COLOR_NAMES = {"red", "lime", "blue", "white"};
    static final int[][] COLOR_YUV = {{82, 90, 240}, {145, 54, 34}, {41, 240, 110}, {235, 128, 128}};

    static String colorOf(int y, int u, int v) {
        for (int i = 0; i < COLOR_YUV.length; i++) {
            if (Math.abs(y - COLOR_YUV[i][0]) <= 10 && Math.abs(u - COLOR_YUV[i][1]) <= 10
                    && Math.abs(v - COLOR_YUV[i][2]) <= 10) {
                return COLOR_NAMES[i];
            }
        }
        return "(" + y + "," + u + "," + v + ")";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // The checks run off the main thread: later ones draw into this window.
        new Thread(() -> {
            try {
                extractor();
                codecList();
                for (String[] clip : CLIPS) decodeSync(clip[0], clip[1]);
                decodeAsync();
                seek();
                audioCodec();
                say("done");
            } catch (Throwable t) {
                say("failed: " + t);
                t.printStackTrace(System.out);
            }
            runOnUiThread(this::finish);
        }, "video-tests").start();
    }

    static final String[][] CLIPS = {
        {"h264", "video_h264.mp4"}, {"hevc", "video_hevc.mp4"}, {"vp8", "video_vp8.webm"},
        {"vp9", "video_vp9.webm"}, {"av1", "video_av1.mp4"},
    };

    MediaExtractor open(String asset) throws Exception {
        MediaExtractor ex = new MediaExtractor();
        try (AssetFileDescriptor afd = getAssets().openFd(asset)) {
            ex.setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
        }
        return ex;
    }

    static int track(MediaExtractor ex, String prefix) {
        for (int i = 0; i < ex.getTrackCount(); i++) {
            String mime = ex.getTrackFormat(i).getString(MediaFormat.KEY_MIME);
            if (mime != null && mime.startsWith(prefix)) return i;
        }
        return -1;
    }

    // --- MediaExtractor ------------------------------------------------------------------

    void extractor() throws Exception {
        MediaExtractor ex = open("video_h264.mp4");
        int v = track(ex, "video/"), a = track(ex, "audio/");
        MediaFormat vf = ex.getTrackFormat(v);
        MediaFormat af = ex.getTrackFormat(a);
        ByteBuffer csd0 = vf.getByteBuffer("csd-0"), csd1 = vf.getByteBuffer("csd-1");
        boolean startCodes = csd0 != null && csd0.remaining() > 4 && csd0.get(0) == 0 && csd0.get(1) == 0
                && csd0.get(2) == 0 && csd0.get(3) == 1 && (csd0.get(4) & 0x1f) == 7;
        ex.selectTrack(v);
        int samples = 0, sync = 0;
        long last = -1;
        boolean annexB = true;
        ByteBuffer buf = ByteBuffer.allocate(1 << 20);
        while (true) {
            int n = ex.readSampleData(buf, 0);
            if (n < 0) break;
            samples++;
            if ((ex.getSampleFlags() & MediaExtractor.SAMPLE_FLAG_SYNC) != 0) sync++;
            last = Math.max(last, ex.getSampleTime());
            if (n < 4 || buf.get(0) != 0 || buf.get(1) != 0) annexB = false;
            ex.advance();
        }
        ex.release();
        say("extractor h264.mp4: " + 2 + " tracks [" + vf.getString(MediaFormat.KEY_MIME) + " "
                + vf.getInteger(MediaFormat.KEY_WIDTH) + "x" + vf.getInteger(MediaFormat.KEY_HEIGHT)
                + ", " + af.getString(MediaFormat.KEY_MIME) + " "
                + af.getInteger(MediaFormat.KEY_SAMPLE_RATE) + " Hz "
                + af.getInteger(MediaFormat.KEY_CHANNEL_COUNT) + " ch], duration "
                + (Math.abs(vf.getLong(MediaFormat.KEY_DURATION) - 2000000) <= 50000 ? "ok" : "bad "
                        + vf.getLong(MediaFormat.KEY_DURATION))
                + ", csd-0 SPS " + startCodes + ", csd-1 " + (csd1 != null && csd1.remaining() > 4)
                + ", " + samples + " samples, " + sync + " sync, Annex B " + annexB
                + ", last at " + (last / 1000) + " ms");
    }

    // --- MediaCodecList ------------------------------------------------------------------

    void codecList() {
        MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        List<String> names = new ArrayList<>();
        for (MediaCodecInfo info : list.getCodecInfos()) {
            names.add(info.getName() + "=" + String.join("+", info.getSupportedTypes()));
        }
        say("codec list (info): " + names);
        String avc = list.findDecoderForFormat(MediaFormat.createVideoFormat("video/avc", 320, 240));
        String aac = list.findDecoderForFormat(MediaFormat.createAudioFormat("audio/mp4a-latm", 44100, 2));
        String enc = list.findEncoderForFormat(MediaFormat.createVideoFormat("video/avc", 320, 240));
        MediaCodecInfo.CodecCapabilities caps = null;
        MediaCodecInfo avcInfo = null;
        for (MediaCodecInfo info : list.getCodecInfos()) {
            if (info.getName().equals(avc)) {
                avcInfo = info;
                caps = info.getCapabilitiesForType("video/avc");
            }
        }
        boolean flexible = false;
        if (caps != null) {
            for (int c : caps.colorFormats) {
                flexible |= c == MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible;
            }
        }
        say("codec list: avc decoder " + avc + ", aac decoder " + aac + ", avc encoder " + enc
                + ", 1920x1080 " + (caps != null && caps.getVideoCapabilities().isSizeSupported(1920, 1080))
                + ", flexible YUV " + flexible + ", profiles " + (caps != null ? caps.profileLevels.length : -1)
                + ", software " + (avcInfo != null && avcInfo.isSoftwareOnly()));
    }

    // --- MediaCodec ----------------------------------------------------------------------

    /** What a decode saw. */
    static class Decoded {
        String codec;
        int frames;
        boolean ordered = true;
        long lastPts = -1;
        MediaFormat outFormat;
        final List<String> colors = new ArrayList<>();
        String image = "none";
        int formatChanges;
    }

    // The frames whose colors are checked: one in each quarter.
    static final int[] PROBE_FRAMES = {7, 22, 37, 52};

    static void probe(Decoded d, ByteBuffer out, MediaFormat f) {
        int w = f.getInteger(MediaFormat.KEY_WIDTH), h = f.getInteger(MediaFormat.KEY_HEIGHT);
        int x = w / 2, y = h / 2;
        int cw = (w + 1) / 2, ch = (h + 1) / 2;
        int base = out.position();
        int Y = out.get(base + y * w + x) & 0xff;
        int U = out.get(base + w * h + (y / 2) * cw + x / 2) & 0xff;
        int V = out.get(base + w * h + cw * ch + (y / 2) * cw + x / 2) & 0xff;
        d.colors.add(colorOf(Y, U, V));
    }

    static void probeImage(Decoded d, Image img) {
        Image.Plane[] p = img.getPlanes();
        int x = img.getWidth() / 2, y = img.getHeight() / 2;
        int Y = p[0].getBuffer().get(y * p[0].getRowStride() + x * p[0].getPixelStride()) & 0xff;
        int U = p[1].getBuffer().get((y / 2) * p[1].getRowStride() + (x / 2) * p[1].getPixelStride()) & 0xff;
        int V = p[2].getBuffer().get((y / 2) * p[2].getRowStride() + (x / 2) * p[2].getPixelStride()) & 0xff;
        d.image = img.getFormat() + " " + img.getWidth() + "x" + img.getHeight() + " " + p.length
                + " planes " + colorOf(Y, U, V);
    }

    void decodeSync(String name, String asset) throws Exception {
        MediaExtractor ex;
        try {
            ex = open(asset);
        } catch (java.io.IOException e) {
            say("codec " + name + " sync: no extractor for " + asset);
            return;
        }
        int v = track(ex, "video/");
        if (v < 0) {
            say("codec " + name + " sync: no video track (no extractor for " + asset + ")");
            ex.release();
            return;
        }
        MediaFormat f = ex.getTrackFormat(v);
        String mime = f.getString(MediaFormat.KEY_MIME);
        String decoder = new MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(f);
        if (decoder == null) {
            say("codec " + name + " sync: no decoder for " + mime);
            ex.release();
            return;
        }
        ex.selectTrack(v);
        MediaCodec codec = MediaCodec.createByCodecName(decoder);
        codec.configure(f, null, null, 0);
        codec.start();
        Decoded d = new Decoded();
        d.codec = codec.getName();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean inputDone = false;
        long deadline = System.currentTimeMillis() + 30000;
        while (System.currentTimeMillis() < deadline) {
            if (!inputDone) {
                int i = codec.dequeueInputBuffer(10000);
                if (i >= 0) {
                    ByteBuffer in = codec.getInputBuffer(i);
                    int n = ex.readSampleData(in, 0);
                    if (n < 0) {
                        codec.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        codec.queueInputBuffer(i, 0, n, ex.getSampleTime(), 0);
                        ex.advance();
                    }
                }
            }
            int o = codec.dequeueOutputBuffer(info, 10000);
            if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                d.outFormat = codec.getOutputFormat();
                d.formatChanges++;
            } else if (o >= 0) {
                if (info.size > 0) {
                    if (info.presentationTimeUs < d.lastPts) d.ordered = false;
                    d.lastPts = info.presentationTimeUs;
                    for (int p : PROBE_FRAMES) {
                        if (p == d.frames) {
                            if (p == PROBE_FRAMES[0]) {
                                Image img = codec.getOutputImage(o);
                                if (img != null) probeImage(d, img);
                            }
                            ByteBuffer out = codec.getOutputBuffer(o);
                            probe(d, out, codec.getOutputFormat(o));
                        }
                    }
                    d.frames++;
                }
                codec.releaseOutputBuffer(o, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        codec.stop();
        codec.release();
        ex.release();
        MediaFormat of = d.outFormat;
        say("codec " + name + " sync: " + d.codec + ", " + (of == null ? "no format" : of.getInteger(MediaFormat.KEY_WIDTH)
                + "x" + of.getInteger(MediaFormat.KEY_HEIGHT) + " color "
                + of.getInteger(MediaFormat.KEY_COLOR_FORMAT))
                + ", " + d.frames + " frames, ordered " + d.ordered + ", colors " + d.colors
                + ", image " + d.image);
    }

    void decodeAsync() throws Exception {
        MediaExtractor ex = open("video_h264.mp4");
        int v = track(ex, "video/");
        MediaFormat f = ex.getTrackFormat(v);
        ex.selectTrack(v);
        MediaCodec codec = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
        HandlerThread thread = new HandlerThread("codec-callbacks");
        thread.start();
        Decoded d = new Decoded();
        CountDownLatch done = new CountDownLatch(1);
        codec.setCallback(new MediaCodec.Callback() {
            boolean inputDone;

            @Override
            public void onInputBufferAvailable(MediaCodec c, int index) {
                if (inputDone) return;
                ByteBuffer in = c.getInputBuffer(index);
                int n = ex.readSampleData(in, 0);
                if (n < 0) {
                    c.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                    inputDone = true;
                } else {
                    c.queueInputBuffer(index, 0, n, ex.getSampleTime(), 0);
                    ex.advance();
                }
            }

            @Override
            public void onOutputBufferAvailable(MediaCodec c, int index, MediaCodec.BufferInfo info) {
                if (info.size > 0) {
                    for (int p : PROBE_FRAMES) {
                        if (p == d.frames) probe(d, c.getOutputBuffer(index), c.getOutputFormat(index));
                    }
                    if (info.presentationTimeUs < d.lastPts) d.ordered = false;
                    d.lastPts = info.presentationTimeUs;
                    d.frames++;
                }
                c.releaseOutputBuffer(index, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) done.countDown();
            }

            @Override
            public void onError(MediaCodec c, MediaCodec.CodecException e) {
                say("codec h264 async: error " + e);
                done.countDown();
            }

            @Override
            public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
                d.formatChanges++;
                d.outFormat = format;
            }
        }, new Handler(thread.getLooper()));
        codec.configure(f, null, null, 0);
        codec.start();
        boolean finished = done.await(30, TimeUnit.SECONDS);
        codec.stop();
        codec.release();
        thread.quitSafely();
        ex.release();
        say("codec h264 async: finished " + finished + ", format changes " + d.formatChanges + ", "
                + d.frames + " frames, ordered " + d.ordered + ", colors " + d.colors);
    }

    /** A seek and a flush: decoding goes on from the key frame at 1 s (blue). */
    void seek() throws Exception {
        MediaExtractor ex = open("video_h264.mp4");
        int v = track(ex, "video/");
        MediaFormat f = ex.getTrackFormat(v);
        ex.selectTrack(v);
        MediaCodec codec = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
        codec.configure(f, null, null, 0);
        codec.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        // A few frames first.
        int got = 0;
        long deadline = System.currentTimeMillis() + 20000;
        while (got < 5 && System.currentTimeMillis() < deadline) {
            int i = codec.dequeueInputBuffer(10000);
            if (i >= 0) {
                int n = ex.readSampleData(codec.getInputBuffer(i), 0);
                codec.queueInputBuffer(i, 0, Math.max(n, 0), ex.getSampleTime(), 0);
                ex.advance();
            }
            int o = codec.dequeueOutputBuffer(info, 10000);
            if (o >= 0) {
                if (info.size > 0) got++;
                codec.releaseOutputBuffer(o, false);
            }
        }
        ex.seekTo(1100000, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
        long seekedTo = ex.getSampleTime();
        codec.flush();
        long firstPts = -1;
        String color = "none";
        boolean inputDone = false;
        deadline = System.currentTimeMillis() + 20000;
        while (firstPts < 0 && System.currentTimeMillis() < deadline) {
            if (!inputDone) {
                int i = codec.dequeueInputBuffer(10000);
                if (i >= 0) {
                    int n = ex.readSampleData(codec.getInputBuffer(i), 0);
                    if (n < 0) {
                        codec.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        codec.queueInputBuffer(i, 0, n, ex.getSampleTime(), 0);
                        ex.advance();
                    }
                }
            }
            int o = codec.dequeueOutputBuffer(info, 10000);
            if (o >= 0) {
                if (info.size > 0) {
                    firstPts = info.presentationTimeUs;
                    Decoded d = new Decoded();
                    probe(d, codec.getOutputBuffer(o), codec.getOutputFormat(o));
                    color = d.colors.get(0);
                }
                codec.releaseOutputBuffer(o, false);
            }
        }
        codec.stop();
        codec.release();
        ex.release();
        say("codec h264 seek: before " + got + " frames, extractor at " + (seekedTo / 1000)
                + " ms, first frame after the flush at " + (firstPts / 1000) + " ms, " + color);
    }

    /** The MP4's AAC track through MediaCodec: PCM of the 1661 Hz tone. */
    void audioCodec() throws Exception {
        MediaExtractor ex = open("video_h264.mp4");
        int a = track(ex, "audio/");
        MediaFormat f = ex.getTrackFormat(a);
        ex.selectTrack(a);
        MediaCodec codec = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
        codec.configure(f, null, null, 0);
        codec.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean inputDone = false;
        long frames = 0, crossings = 0;
        int channels = 0, rate = 0, encoding = 0;
        short prev = 0;
        long deadline = System.currentTimeMillis() + 20000;
        while (System.currentTimeMillis() < deadline) {
            if (!inputDone) {
                int i = codec.dequeueInputBuffer(10000);
                if (i >= 0) {
                    int n = ex.readSampleData(codec.getInputBuffer(i), 0);
                    if (n < 0) {
                        codec.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        codec.queueInputBuffer(i, 0, n, ex.getSampleTime(), 0);
                        ex.advance();
                    }
                }
            }
            int o = codec.dequeueOutputBuffer(info, 10000);
            if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat of = codec.getOutputFormat();
                channels = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                rate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                encoding = of.containsKey(MediaFormat.KEY_PCM_ENCODING)
                        ? of.getInteger(MediaFormat.KEY_PCM_ENCODING) : 2;
            } else if (o >= 0) {
                ByteBuffer out = codec.getOutputBuffer(o);
                if (out != null && channels > 0) {
                    out.order(java.nio.ByteOrder.LITTLE_ENDIAN);
                    int n = info.size / (2 * channels);
                    for (int k = 0; k < n; k++) {
                        short s = out.getShort(info.offset + k * 2 * channels);
                        // Upward crossings, with hysteresis.
                        if (prev < -1000 && s >= 0) crossings++;
                        if (s < -1000 || s >= 0) prev = s;
                    }
                    frames += n;
                }
                codec.releaseOutputBuffer(o, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        codec.stop();
        codec.release();
        ex.release();
        double seconds = rate > 0 ? (double) frames / rate : 0;
        double hz = seconds > 0 ? crossings / seconds : 0;
        say("codec aac: " + rate + " Hz " + channels + " ch, encoding " + encoding + ", "
                + (Math.abs(seconds - 2.0) <= 0.1 ? "2 s" : seconds + " s") + ", "
                + (Math.abs(hz - 1661) <= 1661 * 0.02 ? "1661 Hz" : hz + " Hz"));
    }
}
