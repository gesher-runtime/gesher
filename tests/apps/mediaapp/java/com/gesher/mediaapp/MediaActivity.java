/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.mediaapp;

import android.app.Activity;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFocusRequest;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTimestamp;
import android.media.AudioTrack;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Media the way apps use it (docs/media.md). Every sound it plays is a tone of its own
 * frequency, apart from the next by silence, so the test (tests/windows/tracks/38-media.ps1)
 * finds each one in the audio output's capture and checks its duration, frequency and level;
 * what the APIs report is printed as "MediaTest ..." lines.
 *
 * <p>The sounds, in order (frequency, duration):
 * <ol>
 * <li>AudioTrack streaming, 16-bit mono 44100 Hz: 440 Hz, 0.5 s
 * <li>AudioTrack streaming, 8-bit mono 8000 Hz: 500 Hz, 0.4 s
 * <li>AudioTrack static, float stereo 48000 Hz, a marker: 660 Hz, 0.4 s
 * <li>AudioTrack streaming from a direct ByteBuffer, 16-bit stereo 22050 Hz, volume 0.5:
 *     880 Hz, 0.4 s, half the level
 * <li>AudioTrack static (legacy constructor, STREAM_MUSIC), 16-bit stereo 32000 Hz, looped
 *     twice: 1000 Hz, 3 x 0.2 s
 * <li>AudioTrack streaming, 16-bit mono 24000 Hz played at 48000 Hz (setPlaybackRate):
 *     600 Hz data heard at 1200 Hz, 0.6 s of data in 0.3 s
 * <li>AudioManager: STREAM_MUSIC at volume 7 of 15: 700 Hz, 0.3 s, at 7/15 of the level
 * <li>(none: a notification tone while the ringer is silent)
 * <li>AudioManager: STREAM_NOTIFICATION, the ringer back to normal: 1300 Hz, 0.3 s
 * </ol>
 */
public class MediaActivity extends Activity {
    static final int GAP_MS = 300;

    static void say(String s) {
        System.out.println("MediaTest " + s);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        HandlerThread events = new HandlerThread("media-events");
        events.start();
        try {
            audioTrack(new Handler(events.getLooper()));
            audioManager(new Handler(events.getLooper()));
            say("done");
        } catch (Throwable t) {
            say("failed: " + t);
            t.printStackTrace(System.out);
        } finally {
            events.quitSafely();
        }
    }

    // --- AudioTrack --------------------------------------------------------------------------

    static short[] tone16(int rate, int channels, double hz, int frames, double amplitude) {
        short[] out = new short[frames * channels];
        for (int i = 0; i < frames; i++) {
            short v = (short) Math.round(32767 * amplitude * Math.sin(2 * Math.PI * hz * i / rate));
            for (int c = 0; c < channels; c++) out[i * channels + c] = v;
        }
        return out;
    }

    static byte[] tone8(int rate, double hz, int frames, double amplitude) {
        byte[] out = new byte[frames];
        for (int i = 0; i < frames; i++) {
            out[i] = (byte) (128 + Math.round(127 * amplitude * Math.sin(2 * Math.PI * hz * i / rate)));
        }
        return out;
    }

    static float[] toneFloat(int rate, int channels, double hz, int frames, double amplitude) {
        float[] out = new float[frames * channels];
        for (int i = 0; i < frames; i++) {
            float v = (float) (amplitude * Math.sin(2 * Math.PI * hz * i / rate));
            for (int c = 0; c < channels; c++) out[i * channels + c] = v;
        }
        return out;
    }

    static AudioTrack streaming(int rate, int channelMask, int encoding, int bufferBytes) {
        return new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate)
                        .setChannelMask(channelMask).setEncoding(encoding).build())
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(bufferBytes)
                .build();
    }

    /** Waits until the track has played `frames` frames (streaming, after stop()). */
    static int drain(AudioTrack t, int frames, long timeoutMs) throws InterruptedException {
        long end = System.currentTimeMillis() + timeoutMs;
        int pos = t.getPlaybackHeadPosition();
        while (pos < frames && System.currentTimeMillis() < end) {
            Thread.sleep(10);
            pos = t.getPlaybackHeadPosition();
        }
        return pos;
    }

    void audioTrack(Handler handler) throws Exception {
        AudioManager am = getSystemService(AudioManager.class);
        int nativeRate = AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC);
        int min = AudioTrack.getMinBufferSize(44100, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        say("audiotrack output: native rate " + nativeRate + ", min buffer (44100 mono 16-bit) "
                + (min > 0 ? "ok" : "bad " + min) + ", property rate "
                + am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)
                + ", frames per buffer "
                + (Integer.parseInt(am.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)) > 0
                        ? "ok" : "bad"));

        // 1. Streaming, 16-bit mono 44100 Hz, blocking short writes in pieces.
        {
            int frames = 22050;
            AudioTrack t = streaming(44100, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, Math.max(min, 8820));
            short[] pcm = tone16(44100, 1, 440, frames, 0.5);
            t.play();
            int written = 0;
            while (written < pcm.length) {
                int n = t.write(pcm, written, Math.min(2048, pcm.length - written));
                if (n <= 0) break;
                written += n;
            }
            AudioTimestamp ts = new AudioTimestamp();
            boolean stamped = t.getTimestamp(ts);
            t.stop();
            int pos = drain(t, frames, 3000);
            say("audiotrack 1 streaming 16-bit mono 44100: state " + t.getState() + ", stream "
                    + t.getAudioAttributes().getVolumeControlStream() + ", wrote " + written
                    + ", position " + pos + "/" + frames + ", timestamp " + stamped
                    + ", session " + (t.getAudioSessionId() > 0 ? "ok" : "bad"));
            t.release();
        }
        Thread.sleep(GAP_MS);

        // 2. Streaming, 8-bit mono 8000 Hz, byte writes.
        {
            int frames = 3200;
            AudioTrack t = streaming(8000, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_8BIT, 1600);
            byte[] pcm = tone8(8000, 500, frames, 0.5);
            t.play();
            int written = t.write(pcm, 0, pcm.length);
            t.stop();
            int pos = drain(t, frames, 3000);
            say("audiotrack 2 streaming 8-bit mono 8000: wrote " + written + ", position " + pos
                    + "/" + frames);
            t.release();
        }
        Thread.sleep(GAP_MS);

        // 3. Static, float stereo 48000 Hz, a marker at the middle.
        {
            int frames = 19200;
            float[] pcm = toneFloat(48000, 2, 660, frames, 0.5);
            AudioTrack t = new AudioTrack.Builder()
                    .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                            .setEncoding(AudioFormat.ENCODING_PCM_FLOAT).build())
                    .setTransferMode(AudioTrack.MODE_STATIC)
                    .setBufferSizeInBytes(frames * 8)
                    .build();
            int stateBefore = t.getState();
            int written = t.write(pcm, 0, pcm.length, AudioTrack.WRITE_BLOCKING);
            int stateAfter = t.getState();
            CountDownLatch marker = new CountDownLatch(1);
            AtomicInteger markerAt = new AtomicInteger(-1);
            t.setNotificationMarkerPosition(frames / 2);
            t.setPlaybackPositionUpdateListener(new AudioTrack.OnPlaybackPositionUpdateListener() {
                @Override
                public void onMarkerReached(AudioTrack track) {
                    markerAt.set(track.getNotificationMarkerPosition());
                    marker.countDown();
                }

                @Override
                public void onPeriodicNotification(AudioTrack track) {
                }
            }, handler);
            t.play();
            boolean reached = marker.await(3, TimeUnit.SECONDS);
            int pos = drain(t, frames, 3000);
            t.stop();
            say("audiotrack 3 static float stereo 48000: states " + stateBefore + "->" + stateAfter
                    + ", wrote " + written + ", marker " + reached + " at " + markerAt.get()
                    + ", position " + pos + "/" + frames);
            t.release();
        }
        Thread.sleep(GAP_MS);

        // 4. Streaming from a direct ByteBuffer, 16-bit stereo 22050 Hz, volume 0.5,
        // with periodic position notifications.
        {
            int frames = 8820;
            short[] pcm = tone16(22050, 2, 880, frames, 0.5);
            ByteBuffer buf = ByteBuffer.allocateDirect(pcm.length * 2).order(ByteOrder.nativeOrder());
            buf.asShortBuffer().put(pcm);
            AudioTrack t = streaming(22050, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT, 8820);
            t.setVolume(0.5f);
            AtomicInteger periodic = new AtomicInteger();
            t.setPositionNotificationPeriod(2205);
            t.setPlaybackPositionUpdateListener(new AudioTrack.OnPlaybackPositionUpdateListener() {
                @Override
                public void onMarkerReached(AudioTrack track) {
                }

                @Override
                public void onPeriodicNotification(AudioTrack track) {
                    periodic.incrementAndGet();
                }
            }, handler);
            t.play();
            int written = t.write(buf, buf.remaining(), AudioTrack.WRITE_BLOCKING);
            t.stop();
            int pos = drain(t, frames, 3000);
            Thread.sleep(100);
            say("audiotrack 4 bytebuffer 16-bit stereo 22050 volume 0.5: wrote " + written
                    + ", position " + pos + "/" + frames + ", periodic notifications "
                    + periodic.get() + ", underruns " + (t.getUnderrunCount() >= 0 ? "ok" : "bad"));
            t.release();
        }
        Thread.sleep(GAP_MS);

        // 5. The legacy constructor, STREAM_MUSIC, static, looped twice.
        {
            int frames = 6400;
            short[] pcm = tone16(32000, 2, 1000, frames, 0.5);
            @SuppressWarnings("deprecation")
            AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, 32000,
                    AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT, frames * 4,
                    AudioTrack.MODE_STATIC);
            int written = t.write(pcm, 0, pcm.length);
            int loop = t.setLoopPoints(0, frames, 2);
            t.play();
            Thread.sleep(600 + 400);
            t.stop();
            say("audiotrack 5 legacy static loop: stream " + t.getStreamType() + ", wrote " + written
                    + ", setLoopPoints " + loop);
            t.release();
        }
        Thread.sleep(GAP_MS);

        // 6. Streaming at twice its rate: setPlaybackRate.
        {
            int frames = 14400;
            short[] pcm = tone16(24000, 1, 600, frames, 0.5);
            AudioTrack t = streaming(24000, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, 9600);
            int set = t.setPlaybackRate(48000);
            t.play();
            int written = t.write(pcm, 0, pcm.length);
            t.stop();
            int pos = drain(t, frames, 3000);
            say("audiotrack 6 playback rate: set " + set + ", rate " + t.getPlaybackRate()
                    + ", wrote " + written + ", position " + pos + "/" + frames);
            t.release();
        }
        Thread.sleep(GAP_MS);
    }

    // --- AudioManager ------------------------------------------------------------------------

    /** Plays a 16-bit mono 48000 Hz tone on a track of `usage`, to its end. */
    static void playTone(int usage, double hz, double seconds) throws InterruptedException {
        int frames = (int) (48000 * seconds);
        AudioTrack t = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(usage).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(9600)
                .build();
        short[] pcm = tone16(48000, 1, hz, frames, 0.5);
        t.play();
        t.write(pcm, 0, pcm.length);
        t.stop();
        drain(t, frames, 3000);
        t.release();
    }

    void audioManager(Handler handler) throws Exception {
        AudioManager am = getSystemService(AudioManager.class);
        int music = AudioManager.STREAM_MUSIC;
        say("audiomanager volumes: music " + am.getStreamVolume(music) + "/"
                + am.getStreamMaxVolume(music) + " min " + am.getStreamMinVolume(music)
                + ", ring " + am.getStreamVolume(AudioManager.STREAM_RING) + "/"
                + am.getStreamMaxVolume(AudioManager.STREAM_RING) + ", system follows ring "
                + (am.getStreamVolume(AudioManager.STREAM_SYSTEM)
                        == am.getStreamVolume(AudioManager.STREAM_RING)));

        // Music at 7 of 15: the tone plays at 7/15 of its level.
        am.setStreamVolume(music, 7, 0);
        int set = am.getStreamVolume(music);
        playTone(AudioAttributes.USAGE_MEDIA, 700, 0.3);
        am.adjustStreamVolume(music, AudioManager.ADJUST_RAISE, 0);
        int raised = am.getStreamVolume(music);
        am.adjustStreamVolume(music, AudioManager.ADJUST_MUTE, 0);
        boolean muted = am.isStreamMute(music);
        int mutedVolume = am.getStreamVolume(music);
        am.adjustStreamVolume(music, AudioManager.ADJUST_UNMUTE, 0);
        int unmuted = am.getStreamVolume(music);
        am.setStreamVolume(music, am.getStreamMaxVolume(music), 0);
        say("audiomanager set music 7: " + set + ", raise " + raised + ", mute " + muted + " "
                + mutedVolume + ", unmute " + unmuted + ", back to "
                + am.getStreamVolume(music));
        Thread.sleep(GAP_MS);

        // The ringer: silent mutes the ring and notification streams.
        am.setRingerMode(AudioManager.RINGER_MODE_SILENT);
        say("audiomanager ringer silent: mode " + am.getRingerMode() + ", ring volume "
                + am.getStreamVolume(AudioManager.STREAM_RING) + ", notification muted "
                + am.isStreamMute(AudioManager.STREAM_NOTIFICATION));
        playTone(AudioAttributes.USAGE_NOTIFICATION, 1500, 0.3);  // not heard
        Thread.sleep(GAP_MS);
        am.setRingerMode(AudioManager.RINGER_MODE_NORMAL);
        say("audiomanager ringer normal: mode " + am.getRingerMode() + ", notification volume "
                + am.getStreamVolume(AudioManager.STREAM_NOTIFICATION) + "/"
                + am.getStreamMaxVolume(AudioManager.STREAM_NOTIFICATION));
        playTone(AudioAttributes.USAGE_NOTIFICATION, 1300, 0.3);
        Thread.sleep(GAP_MS);

        // Devices.
        AudioDeviceInfo[] outs = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS);
        AudioDeviceInfo[] ins = am.getDevices(AudioManager.GET_DEVICES_INPUTS);
        StringBuilder d = new StringBuilder();
        for (AudioDeviceInfo info : outs) {
            int[] rates = info.getSampleRates();
            d.append(" [type ").append(info.getType()).append(", sink ").append(info.isSink())
                    .append(", name ").append(info.getProductName())
                    .append(", 48000 Hz ").append(Arrays.stream(rates).anyMatch(r -> r == 48000))
                    .append(", stereo ").append(Arrays.stream(info.getChannelCounts())
                            .anyMatch(c -> c == 2))
                    .append(']');
        }
        say("audiomanager devices: " + outs.length + " output(s)" + d + ", " + ins.length
                + " input(s)");

        // Focus: B takes it from A for a while, gives it back, then takes it for good.
        List<String> changes = Collections.synchronizedList(new ArrayList<>());
        CountDownLatch three = new CountDownLatch(3);
        AudioManager.OnAudioFocusChangeListener a = change -> {
            changes.add("A" + change);
            three.countDown();
        };
        AudioManager.OnAudioFocusChangeListener b = change -> changes.add("B" + change);
        AudioAttributes media = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA).build();
        AudioFocusRequest ra = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(media).setOnAudioFocusChangeListener(a, handler).build();
        AudioFocusRequest rbTransient = new AudioFocusRequest.Builder(
                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                .setAudioAttributes(media).setOnAudioFocusChangeListener(b, handler).build();
        AudioFocusRequest rb = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(media).setOnAudioFocusChangeListener(b, handler).build();
        int g1 = am.requestAudioFocus(ra);
        int g2 = am.requestAudioFocus(rbTransient);
        Thread.sleep(200);
        am.abandonAudioFocusRequest(rbTransient);
        Thread.sleep(200);
        int g3 = am.requestAudioFocus(rb);
        boolean all = three.await(3, TimeUnit.SECONDS);
        am.abandonAudioFocusRequest(rb);
        am.abandonAudioFocusRequest(ra);
        say("audiomanager focus: granted " + g1 + g2 + g3 + ", changes " + changes
                + (all ? "" : " (not all)"));

        // isMusicActive while a track plays.
        AudioTrack t = new AudioTrack.Builder()
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(96000).build();
        short[] silence = new short[48000];
        t.write(silence, 0, silence.length);
        boolean before = am.isMusicActive();
        t.play();
        Thread.sleep(300);
        boolean during = am.isMusicActive();
        t.stop();
        t.release();
        Thread.sleep(500);
        say("audiomanager music active: before " + before + ", playing " + during + ", after "
                + am.isMusicActive());
    }
}
