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
        boolean surfacesOnly = getIntent().getBooleanExtra("surfacesOnly", false);
        // The decoders alone (a run with GESHER_VIDEO_DECODERS=soft: the software ones).
        boolean codecsOnly = getIntent().getBooleanExtra("codecsOnly", false);
        new Thread(() -> {
            try {
                if (codecsOnly) {
                    for (String[] clip : CLIPS) decodeSync(clip[0], clip[1]);
                    say("done");
                    return;
                }
                if (!surfacesOnly) {
                    extractor();
                    codecList();
                    for (String[] clip : CLIPS) decodeSync(clip[0], clip[1]);
                    decodeAsync();
                    seek();
                    audioCodec();
                    webm("vp9_opus", "video_vp9_opus.webm", 880);
                    webm("vp8_vorbis", "video_vp8_vorbis.webm", 660);
                    recordWithoutPermission();
                }
                surfaces();
                if (!surfacesOnly) {
                    retriever();
                    player();
                    videoView();
                    // The software decoders (and WebM's audio) through MediaPlayer and
                    // MediaMetadataRetriever.
                    for (String[] clip : PLAYED) {
                        playClip(clip[0], clip[1]);
                        retrieverClip(clip[0], clip[1]);
                    }
                }
                say("done");
            } catch (Throwable t) {
                say("failed: " + t);
                t.printStackTrace(System.out);
            }
        }, "video-tests").start();
    }

    static final String[][] CLIPS = {
        {"h264", "video_h264.mp4"}, {"hevc", "video_hevc.mp4"}, {"vp8", "video_vp8.webm"},
        {"vp9", "video_vp9.webm"}, {"av1", "video_av1.mp4"},
        {"mpeg4", "video_mpeg4.mp4"}, {"h263", "video_h263.3gp"},
    };

    /** Played by MediaPlayer and read by MediaMetadataRetriever (media3). */
    static final String[][] PLAYED = {
        {"vp9_opus", "video_vp9_opus.webm"}, {"vp8_vorbis", "video_vp8_vorbis.webm"},
        {"vp8", "video_vp8.webm"}, {"av1", "video_av1.mp4"}, {"hevc", "video_hevc.mp4"},
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

    // --- Output surfaces: TextureView and SurfaceView --------------------------------------

    /**
     * Decodes the H.264 clip into `surface`, rendering every frame up to `last` (0-based),
     * then stops: what the surface shows is that frame's color. Returns the frames rendered.
     */
    int decodeToSurface(android.view.Surface surface, int last) throws Exception {
        MediaExtractor ex = open("video_h264.mp4");
        int v = track(ex, "video/");
        MediaFormat f = ex.getTrackFormat(v);
        ex.selectTrack(v);
        MediaCodec codec = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
        codec.configure(f, surface, null, 0);
        codec.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int rendered = 0;
        boolean nullBuffer = true;
        long deadline = System.currentTimeMillis() + 20000;
        while (rendered <= last && System.currentTimeMillis() < deadline) {
            int i = codec.dequeueInputBuffer(10000);
            if (i >= 0) {
                int n = ex.readSampleData(codec.getInputBuffer(i), 0);
                if (n < 0) {
                    codec.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                } else {
                    codec.queueInputBuffer(i, 0, n, ex.getSampleTime(), 0);
                    ex.advance();
                }
            }
            int o = codec.dequeueOutputBuffer(info, 10000);
            if (o >= 0) {
                if (info.size > 0) {
                    nullBuffer &= codec.getOutputBuffer(o) == null;
                    codec.releaseOutputBuffer(o, true);
                    rendered++;
                } else {
                    codec.releaseOutputBuffer(o, false);
                }
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        codec.stop();
        codec.release();
        ex.release();
        if (!nullBuffer) say("surfaces: an output buffer was not null with a Surface");
        return rendered;
    }

    static String rgbName(int argb) {
        int r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
        if (r > 200 && g < 60 && b < 60) return "red";
        if (r < 60 && g > 200 && b < 60) return "lime";
        if (r < 60 && g < 60 && b > 200) return "blue";
        if (r > 200 && g > 200 && b > 200) return "white";
        return String.format("#%06x", argb & 0xffffff);
    }

    /**
     * A TextureView and a SurfaceView, each shown a frame by MediaCodec: the TextureView red
     * (frame 7), the SurfaceView blue (frame 37). TextureView.getBitmap reads its layer back;
     * the test reads the window (PrintWindow) while this waits.
     */
    void surfaces() throws Exception {
        final android.graphics.SurfaceTexture[] texture = new android.graphics.SurfaceTexture[1];
        final android.view.Surface[] holderSurface = new android.view.Surface[1];
        final android.view.TextureView[] tv = new android.view.TextureView[1];
        final android.view.SurfaceView[] sv = new android.view.SurfaceView[1];
        CountDownLatch ready = new CountDownLatch(2);
        try {
            // SkiaGL: keep a copy of each GL frame (GraphicsDebug.lastGlFrame below).
            Class.forName("com.gesher.app.graphics.GraphicsDebug").getMethod("setGlReadback", boolean.class)
                    .invoke(null, true);
        } catch (Throwable t) {
            // Not on Gesher's boot class path: nothing to read back.
        }
        runOnUiThread(() -> {
            android.widget.FrameLayout root = new android.widget.FrameLayout(this);
            root.setBackgroundColor(0xff808080);
            tv[0] = new android.view.TextureView(this);
            tv[0].setSurfaceTextureListener(new android.view.TextureView.SurfaceTextureListener() {
                @Override
                public void onSurfaceTextureAvailable(android.graphics.SurfaceTexture s, int w, int h) {
                    texture[0] = s;
                    ready.countDown();
                }

                @Override
                public void onSurfaceTextureSizeChanged(android.graphics.SurfaceTexture s, int w, int h) {}

                @Override
                public boolean onSurfaceTextureDestroyed(android.graphics.SurfaceTexture s) {
                    return true;
                }

                @Override
                public void onSurfaceTextureUpdated(android.graphics.SurfaceTexture s) {}
            });
            android.widget.FrameLayout.LayoutParams tlp = new android.widget.FrameLayout.LayoutParams(320, 240);
            tlp.leftMargin = 40;
            tlp.topMargin = 60;
            root.addView(tv[0], tlp);
            sv[0] = new android.view.SurfaceView(this);
            sv[0].getHolder().addCallback(new android.view.SurfaceHolder.Callback() {
                @Override
                public void surfaceCreated(android.view.SurfaceHolder h) {
                    holderSurface[0] = h.getSurface();
                    ready.countDown();
                }

                @Override
                public void surfaceChanged(android.view.SurfaceHolder h, int format, int w, int hh) {}

                @Override
                public void surfaceDestroyed(android.view.SurfaceHolder h) {}
            });
            android.widget.FrameLayout.LayoutParams slp = new android.widget.FrameLayout.LayoutParams(320, 240);
            slp.leftMargin = 40;
            slp.topMargin = 400;
            root.addView(sv[0], slp);
            setContentView(root);
        });
        boolean created = ready.await(20, TimeUnit.SECONDS);
        if (!created) {
            say("surfaces: texture " + (texture[0] != null) + ", surface view surface "
                    + (holderSurface[0] != null) + " (not created within 20 s)");
            return;
        }
        android.view.Surface textureSurface = new android.view.Surface(texture[0]);
        int toTexture = decodeToSurface(textureSurface, 7);
        int toView = decodeToSurface(holderSurface[0], 37);
        Thread.sleep(1500);
        final String[] bitmap = new String[1];
        final int[][] where = new int[2][];
        CountDownLatch read = new CountDownLatch(1);
        runOnUiThread(() -> {
            android.graphics.Bitmap b = tv[0].getBitmap();
            bitmap[0] = b == null ? "null" : b.getWidth() + "x" + b.getHeight() + " "
                    + rgbName(b.getPixel(b.getWidth() / 2, b.getHeight() / 2));
            where[0] = new int[2];
            where[1] = new int[2];
            tv[0].getLocationInWindow(where[0]);
            sv[0].getLocationInWindow(where[1]);
            read.countDown();
        });
        read.await(10, TimeUnit.SECONDS);
        say("surfaces: texture view " + toTexture + " frames rendered, getBitmap " + bitmap[0]
                + "; surface view valid " + holderSurface[0].isValid() + ", " + toView + " frames rendered");
        // The test reads the window now: the views' centers in window coordinates.
        String renderer;
        try {
            renderer = (String) Class.forName("com.gesher.app.graphics.GraphicsDebug")
                    .getMethod("renderer").invoke(null);
        } catch (Throwable t) {
            renderer = "unknown";
        }
        say("surfaces renderer: " + renderer);
        if ("skiagl".equals(renderer)) {
            // The last GL frame hwui presented (glReadPixels before eglSwapBuffers): what
            // the SkiaGL pipeline drew, whatever Windows shows of the window.
            try {
                final long[] hwnd = new long[1];
                CountDownLatch got = new CountDownLatch(1);
                runOnUiThread(() -> {
                    try {
                        Object session = Class.forName("com.gesher.app.window.WindowSession")
                                .getMethod("get").invoke(null);
                        android.os.IBinder token = getWindow().getDecorView().getWindowToken();
                        hwnd[0] = (Long) session.getClass().getMethod("hwndOf", android.os.IBinder.class)
                                .invoke(session, token);
                    } catch (Throwable t) {
                        say("surfaces gl frame: no window handle (" + t + ")");
                    }
                    got.countDown();
                });
                got.await(5, TimeUnit.SECONDS);
                int[] frame = (int[]) Class.forName("com.gesher.app.graphics.GraphicsDebug")
                        .getMethod("lastGlFrame", long.class).invoke(null, hwnd[0]);
                if (frame == null) {
                    say("surfaces gl frame: none (hwnd 0x" + Long.toHexString(hwnd[0]) + ")");
                } else {
                    int fw = frame[0];
                    java.util.function.BiFunction<Integer, Integer, String> at =
                            (x, y) -> String.format("%06x", frame[2 + y * fw + x] & 0xffffff);
                    say("surfaces gl frame: texture view #" + at.apply(where[0][0] + 160, where[0][1] + 120)
                            + ", surface view #" + at.apply(where[1][0] + 160, where[1][1] + 120)
                            + ", background #" + at.apply(10, 300));
                }
            } catch (Throwable t) {
                say("surfaces gl frame: " + t);
            }
        }
        say("surfaces ready: texture view center " + (where[0][0] + 160) + "," + (where[0][1] + 120)
                + ", surface view center " + (where[1][0] + 160) + "," + (where[1][1] + 120));
        Thread.sleep(10000);
        textureSurface.release();
    }

    // --- MediaMetadataRetriever, MediaPlayer with video, VideoView ----------------------------

    java.io.File clipFile() throws Exception {
        java.io.File out = new java.io.File(getFilesDir(), "video_h264.mp4");
        if (!out.exists()) {
            try (java.io.InputStream in = getAssets().open("video_h264.mp4");
                 java.io.FileOutputStream os = new java.io.FileOutputStream(out)) {
                in.transferTo(os);
            }
        }
        return out;
    }

    void retriever() throws Exception {
        android.media.MediaMetadataRetriever r = new android.media.MediaMetadataRetriever();
        r.setDataSource(clipFile().getPath());
        android.graphics.Bitmap closest = r.getFrameAtTime(1250000,
                android.media.MediaMetadataRetriever.OPTION_CLOSEST);
        android.graphics.Bitmap sync = r.getFrameAtTime(1600000,
                android.media.MediaMetadataRetriever.OPTION_PREVIOUS_SYNC);
        android.graphics.Bitmap scaled = r.getScaledFrameAtTime(250000,
                android.media.MediaMetadataRetriever.OPTION_CLOSEST, 160, 160);
        String w = r.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH);
        String h = r.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT);
        String hasVideo = r.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_HAS_VIDEO);
        r.release();
        say("retriever frames: video " + w + "x" + h + " " + hasVideo + ", at 1.25 s closest "
                + describe(closest) + ", at 1.6 s previous sync " + describe(sync)
                + ", scaled at 0.25 s " + describe(scaled));
    }

    static String describe(android.graphics.Bitmap b) {
        if (b == null) return "null";
        return b.getWidth() + "x" + b.getHeight() + " " + rgbName(b.getPixel(b.getWidth() / 2, b.getHeight() / 2));
    }

    /**
     * MediaPlayer playing the clip (video and its AAC audio) into a TextureView: the video
     * size, the frames reaching the view, and A/V sync: while it plays, the frame shown
     * (TextureView.getBitmap's color: which half second) is checked against the audio's
     * position (getCurrentPosition), away from the color changes.
     */
    void player() throws Exception {
        final android.graphics.SurfaceTexture[] texture = new android.graphics.SurfaceTexture[1];
        final android.view.TextureView[] tv = new android.view.TextureView[1];
        final java.util.concurrent.atomic.AtomicInteger updates = new java.util.concurrent.atomic.AtomicInteger();
        CountDownLatch ready = new CountDownLatch(1);
        runOnUiThread(() -> {
            android.widget.FrameLayout root = new android.widget.FrameLayout(this);
            tv[0] = new android.view.TextureView(this);
            tv[0].setSurfaceTextureListener(new android.view.TextureView.SurfaceTextureListener() {
                @Override
                public void onSurfaceTextureAvailable(android.graphics.SurfaceTexture s, int w, int h) {
                    texture[0] = s;
                    ready.countDown();
                }

                @Override
                public void onSurfaceTextureSizeChanged(android.graphics.SurfaceTexture s, int w, int h) {}

                @Override
                public boolean onSurfaceTextureDestroyed(android.graphics.SurfaceTexture s) {
                    return true;
                }

                @Override
                public void onSurfaceTextureUpdated(android.graphics.SurfaceTexture s) {
                    updates.incrementAndGet();
                }
            });
            root.addView(tv[0], new android.widget.FrameLayout.LayoutParams(320, 240));
            setContentView(root);
        });
        if (!ready.await(20, TimeUnit.SECONDS)) {
            say("player video: no SurfaceTexture");
            return;
        }
        android.media.MediaPlayer mp = new android.media.MediaPlayer();
        final int[] size = new int[2];
        CountDownLatch completed = new CountDownLatch(1);
        mp.setOnVideoSizeChangedListener((p, w, h) -> {
            size[0] = w;
            size[1] = h;
        });
        mp.setOnCompletionListener(p -> completed.countDown());
        android.view.Surface surface = new android.view.Surface(texture[0]);
        mp.setSurface(surface);
        mp.setDataSource(clipFile().getPath());
        mp.prepare();
        int vw = mp.getVideoWidth(), vh = mp.getVideoHeight(), duration = mp.getDuration();
        updates.set(0);
        mp.start();
        int checked = 0, wrong = 0;
        StringBuilder misses = new StringBuilder();
        long end = System.currentTimeMillis() + 2600;
        while (System.currentTimeMillis() < end && completed.getCount() > 0) {
            Thread.sleep(90);
            int pos = mp.getCurrentPosition();
            final String[] color = new String[1];
            CountDownLatch read = new CountDownLatch(1);
            runOnUiThread(() -> {
                android.graphics.Bitmap b = tv[0].getBitmap();
                color[0] = b == null ? "null" : rgbName(b.getPixel(b.getWidth() / 2, b.getHeight() / 2));
                read.countDown();
            });
            read.await(2, TimeUnit.SECONDS);
            int pos2 = mp.getCurrentPosition();
            int at = (pos + pos2) / 2;
            int quarter = at / 500, into = at % 500;
            if (quarter < 4 && into > 120 && into < 380) {
                checked++;
                if (!COLOR_NAMES[quarter].equals(color[0])) {
                    wrong++;
                    misses.append(" ").append(at).append("ms=").append(color[0]);
                }
            }
        }
        boolean done = completed.await(5, TimeUnit.SECONDS);
        int finalPos = mp.getCurrentPosition();
        mp.release();
        surface.release();
        say("player video: size " + vw + "x" + vh + " (listener " + size[0] + "x" + size[1] + "), duration "
                + (Math.abs(duration - 2000) <= 50 ? "ok" : duration) + ", frames shown "
                + (updates.get() >= 30 ? "30+" : String.valueOf(updates.get())) + ", completed " + done
                + ", at the end " + (Math.abs(finalPos - duration) <= 100 ? "ok" : finalPos));
        say("player sync: " + checked + " checks, " + (checked >= 8 && wrong == 0 ? "in sync"
                : wrong + " off" + misses));
    }

    /** VideoView: a path, start, completion. */
    void videoView() throws Exception {
        final android.widget.VideoView[] vv = new android.widget.VideoView[1];
        CountDownLatch prepared = new CountDownLatch(1), completed = new CountDownLatch(1);
        final boolean[] playing = new boolean[1];
        final int[] duration = new int[1];
        final java.io.File file = clipFile();
        runOnUiThread(() -> {
            vv[0] = new android.widget.VideoView(this);
            vv[0].setOnPreparedListener(mpPrepared -> {
                duration[0] = mpPrepared.getDuration();
                prepared.countDown();
            });
            vv[0].setOnCompletionListener(mpDone -> completed.countDown());
            android.widget.FrameLayout root = new android.widget.FrameLayout(this);
            root.addView(vv[0], new android.widget.FrameLayout.LayoutParams(320, 240));
            setContentView(root);
            vv[0].setVideoPath(file.getPath());
            vv[0].start();
        });
        boolean p = prepared.await(20, TimeUnit.SECONDS);
        Thread.sleep(800);
        CountDownLatch read = new CountDownLatch(1);
        runOnUiThread(() -> {
            playing[0] = vv[0].isPlaying();
            read.countDown();
        });
        read.await(5, TimeUnit.SECONDS);
        boolean c = completed.await(10, TimeUnit.SECONDS);
        runOnUiThread(() -> vv[0].stopPlayback());
        say("video view: prepared " + p + ", duration " + (Math.abs(duration[0] - 2000) <= 50 ? "ok" : duration[0])
                + ", playing " + playing[0] + ", completed " + c);
    }

    // --- media3: WebM with audio, the software decoders through MediaPlayer --------------

    java.io.File fileOf(String asset) throws Exception {
        java.io.File out = new java.io.File(getFilesDir(), asset);
        if (!out.exists()) {
            try (java.io.InputStream in = getAssets().open(asset);
                 java.io.FileOutputStream os = new java.io.FileOutputStream(out)) {
                in.transferTo(os);
            }
        }
        return out;
    }

    /**
     * A WebM clip with an audio track (MatroskaExtractor): its tracks, its video samples,
     * a seek to the previous sync sample, and the audio track decoded by MediaCodec.
     */
    void webm(String name, String asset, int hz) throws Exception {
        MediaExtractor ex = open(asset);
        int v = track(ex, "video/"), a = track(ex, "audio/");
        if (v < 0 || a < 0) {
            say("webm " + name + ": tracks video " + v + ", audio " + a + " (" + ex.getTrackCount() + ")");
            ex.release();
            return;
        }
        MediaFormat vf = ex.getTrackFormat(v), af = ex.getTrackFormat(a);
        ex.selectTrack(v);
        int samples = 0, sync = 0;
        ByteBuffer buf = ByteBuffer.allocate(1 << 20);
        while (ex.readSampleData(buf, 0) >= 0) {
            samples++;
            if ((ex.getSampleFlags() & MediaExtractor.SAMPLE_FLAG_SYNC) != 0) sync++;
            ex.advance();
        }
        ex.seekTo(1100000, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
        long previous = ex.getSampleTime();
        boolean previousSync = (ex.getSampleFlags() & MediaExtractor.SAMPLE_FLAG_SYNC) != 0;
        ex.seekTo(1100000, MediaExtractor.SEEK_TO_NEXT_SYNC);
        long next = ex.getSampleTime();
        ex.seekTo(0, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
        long start = ex.getSampleTime();
        ex.unselectTrack(v);
        ex.selectTrack(a);
        ex.seekTo(0, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
        String audio = decodeAudio(ex, af, hz);
        ex.release();
        long duration = vf.containsKey(MediaFormat.KEY_DURATION) ? vf.getLong(MediaFormat.KEY_DURATION) : -1;
        say("webm " + name + ": tracks [" + vf.getString(MediaFormat.KEY_MIME) + " "
                + vf.getInteger(MediaFormat.KEY_WIDTH) + "x" + vf.getInteger(MediaFormat.KEY_HEIGHT) + ", "
                + af.getString(MediaFormat.KEY_MIME) + " " + af.getInteger(MediaFormat.KEY_SAMPLE_RATE) + " Hz "
                + af.getInteger(MediaFormat.KEY_CHANNEL_COUNT) + " ch], duration "
                + (Math.abs(duration - 2000000) <= 60000 ? "ok" : String.valueOf(duration))
                + "; seek to 1.1 s: previous sync at " + Math.round(previous / 10000.0) * 10 + " ms (sync "
                + previousSync + "), back to " + Math.round(start / 10000.0) * 10 + " ms; audio " + audio);
        say("webm " + name + " (info): " + samples + " video samples, " + sync
                + " sync, next sync after 1.1 s at " + (next / 1000) + " ms");
    }

    /** The selected audio track through MediaCodec: "R Hz C ch, 2 s, H Hz". */
    String decodeAudio(MediaExtractor ex, MediaFormat f, int expectHz) throws Exception {
        MediaCodec codec = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
        codec.configure(f, null, null, 0);
        codec.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean inputDone = false;
        long frames = 0, crossings = 0;
        int channels = 0, rate = 0;
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
            } else if (o >= 0) {
                ByteBuffer out = codec.getOutputBuffer(o);
                if (out != null && channels > 0) {
                    out.order(java.nio.ByteOrder.LITTLE_ENDIAN);
                    int n = info.size / (2 * channels);
                    for (int k = 0; k < n; k++) {
                        short s = out.getShort(info.offset + k * 2 * channels);
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
        double seconds = rate > 0 ? (double) frames / rate : 0;
        double hz = seconds > 0 ? crossings / seconds : 0;
        return rate + " Hz " + channels + " ch, " + (Math.abs(seconds - 2.0) <= 0.1 ? "2 s" : seconds + " s")
                + ", " + (Math.abs(hz - expectHz) <= expectHz * 0.02 ? expectHz + " Hz" : hz + " Hz");
    }

    /** MediaPlayer plays a clip (video, and audio where it has some) into a TextureView. */
    void playClip(String name, String asset) throws Exception {
        final android.graphics.SurfaceTexture[] texture = new android.graphics.SurfaceTexture[1];
        final android.view.TextureView[] tv = new android.view.TextureView[1];
        final java.util.concurrent.atomic.AtomicInteger updates = new java.util.concurrent.atomic.AtomicInteger();
        CountDownLatch ready = new CountDownLatch(1);
        runOnUiThread(() -> {
            android.widget.FrameLayout root = new android.widget.FrameLayout(this);
            tv[0] = new android.view.TextureView(this);
            tv[0].setSurfaceTextureListener(new android.view.TextureView.SurfaceTextureListener() {
                @Override
                public void onSurfaceTextureAvailable(android.graphics.SurfaceTexture s, int w, int h) {
                    texture[0] = s;
                    ready.countDown();
                }

                @Override
                public void onSurfaceTextureSizeChanged(android.graphics.SurfaceTexture s, int w, int h) {}

                @Override
                public boolean onSurfaceTextureDestroyed(android.graphics.SurfaceTexture s) {
                    return true;
                }

                @Override
                public void onSurfaceTextureUpdated(android.graphics.SurfaceTexture s) {
                    updates.incrementAndGet();
                }
            });
            root.addView(tv[0], new android.widget.FrameLayout.LayoutParams(320, 240));
            setContentView(root);
        });
        if (!ready.await(20, TimeUnit.SECONDS)) {
            say("player " + name + ": no SurfaceTexture");
            return;
        }
        android.media.MediaPlayer mp = new android.media.MediaPlayer();
        CountDownLatch completed = new CountDownLatch(1);
        mp.setOnCompletionListener(p -> completed.countDown());
        android.view.Surface surface = new android.view.Surface(texture[0]);
        mp.setSurface(surface);
        String result;
        try {
            mp.setDataSource(fileOf(asset).getPath());
            mp.prepare();
            int vw = mp.getVideoWidth(), vh = mp.getVideoHeight(), duration = mp.getDuration();
            updates.set(0);
            mp.start();
            Thread.sleep(1250);
            final String[] color = new String[1];
            CountDownLatch read = new CountDownLatch(1);
            runOnUiThread(() -> {
                android.graphics.Bitmap b = tv[0].getBitmap();
                color[0] = b == null ? "null" : rgbName(b.getPixel(b.getWidth() / 2, b.getHeight() / 2));
                read.countDown();
            });
            read.await(2, TimeUnit.SECONDS);
            boolean done = completed.await(8, TimeUnit.SECONDS);
            result = "size " + vw + "x" + vh + ", duration " + (Math.abs(duration - 2000) <= 60 ? "ok" : duration)
                    + ", frames shown " + (updates.get() >= 30 ? "30+" : String.valueOf(updates.get()))
                    + ", completed " + done + " (at 1.25 s: " + color[0] + ")";
        } catch (Exception e) {
            result = "failed: " + e;
        }
        mp.release();
        surface.release();
        say("player " + name + ": " + result);
    }

    /** MediaMetadataRetriever on a clip: its video, its audio, a frame at 1.25 s (blue). */
    void retrieverClip(String name, String asset) {
        String result;
        android.media.MediaMetadataRetriever r = new android.media.MediaMetadataRetriever();
        try {
            r.setDataSource(fileOf(asset).getPath());
            android.graphics.Bitmap frame = r.getFrameAtTime(1250000,
                    android.media.MediaMetadataRetriever.OPTION_CLOSEST);
            String w = r.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH);
            String h = r.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT);
            String audio = r.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_HAS_AUDIO);
            String mime = r.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_MIMETYPE);
            result = "video " + w + "x" + h + ", audio " + audio + ", at 1.25 s closest " + describe(frame)
                    + " (mime " + mime + ")";
        } catch (Exception e) {
            result = "failed: " + e;
        }
        try {
            r.release();
        } catch (Exception e) {
            // nothing
        }
        say("retriever " + name + ": " + result);
    }

    /**
     * media3: this app does not ask for RECORD_AUDIO, so recording is refused, as on a phone:
     * the AudioRecord stays uninitialized, MediaRecorder.setAudioSource throws.
     */
    void recordWithoutPermission() {
        android.media.AudioRecord r = new android.media.AudioRecord(
                android.media.MediaRecorder.AudioSource.MIC, 44100,
                android.media.AudioFormat.CHANNEL_IN_MONO, android.media.AudioFormat.ENCODING_PCM_16BIT, 8820);
        int state = r.getState();
        String start;
        try {
            r.startRecording();
            start = "started";
        } catch (IllegalStateException e) {
            start = "IllegalStateException";
        }
        r.release();
        String source;
        android.media.MediaRecorder mr = new android.media.MediaRecorder(this);
        try {
            mr.setAudioSource(android.media.MediaRecorder.AudioSource.MIC);
            source = "set";
        } catch (RuntimeException e) {
            source = e.getClass().getSimpleName();
        }
        mr.release();
        say("record without RECORD_AUDIO: AudioRecord state " + state + ", startRecording " + start
                + ", MediaRecorder.setAudioSource " + source);
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
