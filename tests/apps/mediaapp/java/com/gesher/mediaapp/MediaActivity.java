/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.mediaapp;

import android.app.Activity;
import android.content.res.AssetFileDescriptor;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFocusRequest;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTimestamp;
import android.media.AudioTrack;
import android.media.MediaMetadataRetriever;
import android.media.MediaPlayer;
import android.media.Ringtone;
import android.media.RingtoneManager;
import android.net.Uri;
import android.media.SoundPool;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.FutureTask;
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
 * <li>MediaPlayer.create, a raw MP3: 523 Hz, 0.8 s
 * <li>MediaPlayer, an M4A (AAC) asset: 587 Hz, 0.8 s
 * <li>MediaPlayer, a raw Ogg Vorbis: 659 Hz, 0.8 s
 * <li>MediaPlayer, an Opus asset: 698 Hz, 0.8 s
 * <li>MediaPlayer, a FLAC asset: 784 Hz, 0.8 s
 * <li>MediaPlayer, a WAV by path: 880 Hz, 0.8 s
 * <li>MediaPlayer, an ADTS AAC asset: 988 Hz, 0.8 s
 * <li>MediaPlayer, a G.711 mu-law WAV asset: 1047 Hz, 0.8 s
 * <li>MediaPlayer, 24-bit WAV: 330 Hz, 0.3 s; paused; then from 1.5 s of 2 s: 330 Hz, 0.5 s
 * <li>MediaPlayer, looping the WAV twice: 880 Hz, 1.6 s
 * <li>MediaPlayer, http: 1175 Hz, 0.8 s
 * <li>SoundPool, the WAV: 880 Hz, 0.8 s; the Vorbis tone at rate 2: 1318 Hz, 0.4 s
 * <li>Ringtone, the default notification sound (Gesher's): 1319 Hz, dying away over 0.6 s
 * <li>AAudio, a data callback stream (float stereo): 1397 Hz, 0.5 s
 * <li>AAudio, a blocking-write stream (16-bit mono 22050 Hz): 1480 Hz, 0.4 s
 * <li>OpenSL ES, a buffer queue player at -6 dB: 1568 Hz, 0.6 s, half the level
 * <li>OpenSL ES, a player of the raw Ogg Vorbis by file descriptor: 659 Hz, 0.8 s
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
            mediaPlayer(new Handler(events.getLooper()));
            ringtones();
            ndkAudio();
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

        // 7. AudioTrack.Builder's defaults (Signal Generator's track): no sample rate (the
        // output's), no buffer size (a single frame asked for; the minimum given).
        {
            AudioTrack t = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
                    .build();
            int rate = t.getSampleRate();
            int frames = rate * 3 / 10;
            short[] pcm = tone16(rate, 1, 750, frames, 0.5);
            int minFrames = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT) / 2;
            boolean bufferOk = t.getBufferSizeInFrames() >= minFrames;
            t.play();
            long t0 = System.nanoTime();
            int written = t.write(pcm, 0, pcm.length);
            long ms = (System.nanoTime() - t0) / 1000000;
            t.stop();
            int pos = drain(t, frames, 3000);
            say("audiotrack 7 builder defaults: rate " + rate + ", buffer at least the minimum "
                    + bufferOk + ", wrote " + written + " in time " + (ms < 1000) + ", position "
                    + pos + "/" + frames);
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

    // --- MediaPlayer, MediaMetadataRetriever, SoundPool --------------------------------------

    /** Runs `c` on the thread of `h` (MediaPlayer and SoundPool post events to its looper). */
    static <T> T on(Handler h, Callable<T> c) throws Exception {
        FutureTask<T> f = new FutureTask<>(c);
        h.post(f);
        return f.get(10, TimeUnit.SECONDS);
    }

    /** A player's events, counted. */
    static final class Events {
        final CountDownLatch prepared = new CountDownLatch(1);
        final CountDownLatch completed = new CountDownLatch(1);
        final CountDownLatch seeked = new CountDownLatch(1);
        final List<String> errors = Collections.synchronizedList(new ArrayList<>());

        void attach(MediaPlayer mp) {
            mp.setOnPreparedListener(p -> prepared.countDown());
            mp.setOnCompletionListener(p -> completed.countDown());
            mp.setOnSeekCompleteListener(p -> seeked.countDown());
            mp.setOnErrorListener((p, what, extra) -> {
                errors.add(what + "/" + extra);
                return true;
            });
        }
    }

    /** Plays a prepared player to its end; its duration and its position at the end. */
    static String playToEnd(MediaPlayer mp, Events ev) throws InterruptedException {
        int duration = mp.getDuration();
        mp.start();
        boolean playing = mp.isPlaying();
        boolean done = ev.completed.await(6, TimeUnit.SECONDS);
        String r = "duration " + (Math.abs(duration - 800) <= 60 ? "ok" : "bad " + duration)
                + ", playing " + playing + ", completed " + done + ", at end "
                + (Math.abs(mp.getCurrentPosition() - duration) <= 60 ? "ok" : "bad "
                        + mp.getCurrentPosition())
                + (ev.errors.isEmpty() ? "" : ", errors " + ev.errors);
        mp.release();
        return r;
    }

    void mediaPlayer(Handler handler) throws Exception {
        // 9. MediaPlayer.create (a raw resource, prepared): MP3.
        {
            Events ev = new Events();
            MediaPlayer mp = on(handler, () -> MediaPlayer.create(this, R.raw.tone_mp3));
            if (mp == null) {
                say("mediaplayer mp3 (create, raw): create failed");
            } else {
                ev.attach(mp);
                say("mediaplayer mp3 (create, raw): " + playToEnd(mp, ev));
            }
        }
        Thread.sleep(GAP_MS);
        // 10-13, 15-16. From assets and raw resources, prepareAsync.
        String[][] files = {
            {"aac (m4a, asset)", "tone_aac.m4a"},
            {"vorbis (ogg, raw)", "raw:" + R.raw.tone_vorbis},
            {"opus (asset)", "tone_opus.opus"},
            {"flac (asset)", "tone_flac.flac"},
            {"wav (file path)", "file:tone_wav"},
            {"aac (adts, asset)", "tone_adts.aac"},
            {"g711 mu-law (wav, asset)", "tone_ulaw.wav"},
        };
        for (String[] f : files) {
            Events ev = new Events();
            MediaPlayer mp = on(handler, MediaPlayer::new);
            ev.attach(mp);
            try {
                if (f[1].startsWith("raw:")) {
                    try (AssetFileDescriptor afd = getResources().openRawResourceFd(
                            Integer.parseInt(f[1].substring(4)))) {
                        mp.setDataSource(afd);
                    }
                } else if (f[1].startsWith("file:")) {
                    File out = new File(getFilesDir(), "tone_wav.wav");
                    try (InputStream in = getResources().openRawResource(R.raw.tone_wav);
                         FileOutputStream os = new FileOutputStream(out)) {
                        in.transferTo(os);
                    }
                    mp.setDataSource(out.getPath());
                } else {
                    try (AssetFileDescriptor afd = getAssets().openFd(f[1])) {
                        mp.setDataSource(afd);
                    }
                }
                mp.prepareAsync();
                boolean prepared = ev.prepared.await(5, TimeUnit.SECONDS);
                say("mediaplayer " + f[0] + ": prepared " + prepared + ", " + playToEnd(mp, ev));
            } catch (Exception e) {
                say("mediaplayer " + f[0] + ": " + e);
                mp.release();
            }
            Thread.sleep(GAP_MS);
        }

        // 17. Pause, resume and seek: 330 Hz, 2 s; 0.3 s, a pause, then from 1.5 s to the end.
        {
            Events ev = new Events();
            MediaPlayer mp = on(handler, MediaPlayer::new);
            ev.attach(mp);
            try (AssetFileDescriptor afd = getAssets().openFd("long_wav24.wav")) {
                mp.setDataSource(afd);
            }
            mp.prepare();
            int duration = mp.getDuration();
            mp.start();
            Thread.sleep(300);
            mp.pause();
            int paused = mp.getCurrentPosition();
            Thread.sleep(400);
            int stillPaused = mp.getCurrentPosition();
            boolean playingWhilePaused = mp.isPlaying();
            mp.seekTo(1500);
            boolean seeked = ev.seeked.await(3, TimeUnit.SECONDS);
            int afterSeek = mp.getCurrentPosition();
            mp.start();
            boolean done = ev.completed.await(5, TimeUnit.SECONDS);
            say("mediaplayer seek (24-bit wav): duration " + duration + ", paused at "
                    + (paused >= 200 && paused <= 450 ? "ok" : "bad " + paused) + ", stays "
                    + (Math.abs(stillPaused - paused) <= 20) + ", playing " + playingWhilePaused
                    + ", seek complete " + seeked + " at " + afterSeek + ", completed " + done
                    + (ev.errors.isEmpty() ? "" : ", errors " + ev.errors));
            mp.release();
        }
        Thread.sleep(GAP_MS);

        // 18. Looping: the WAV around twice, then to its end (880 Hz, 1.6 s in one piece).
        {
            Events ev = new Events();
            MediaPlayer mp = on(handler, MediaPlayer::new);
            ev.attach(mp);
            try (AssetFileDescriptor afd = getResources().openRawResourceFd(R.raw.tone_wav)) {
                mp.setDataSource(afd);
            }
            mp.prepare();
            mp.setLooping(true);
            mp.start();
            Thread.sleep(1200);
            boolean looping = mp.isLooping();
            boolean stillPlaying = mp.isPlaying();
            mp.setLooping(false);
            boolean done = ev.completed.await(3, TimeUnit.SECONDS);
            say("mediaplayer looping: looping " + looping + ", playing after 1.2 s " + stillPlaying
                    + ", completed after setLooping(false) " + done);
            mp.release();
        }
        Thread.sleep(GAP_MS);

        // 19. http: Ogg Vorbis from the test's server (127.0.0.1:8765), 1175 Hz.
        {
            Events ev = new Events();
            MediaPlayer mp = on(handler, MediaPlayer::new);
            ev.attach(mp);
            try {
                mp.setDataSource("http://127.0.0.1:8765/tone_http.ogg");
                mp.prepareAsync();
                boolean prepared = ev.prepared.await(10, TimeUnit.SECONDS);
                say("mediaplayer http (ogg): prepared " + prepared + ", " + playToEnd(mp, ev));
            } catch (Exception e) {
                say("mediaplayer http (ogg): " + e);
                mp.release();
            }
        }
        Thread.sleep(GAP_MS);

        // Errors: start() before a data source; a file that is no media.
        {
            Events ev = new Events();
            MediaPlayer mp = on(handler, MediaPlayer::new);
            ev.attach(mp);
            mp.start();
            Thread.sleep(200);
            String early = ev.errors.toString();
            mp.reset();
            File junk = new File(getFilesDir(), "junk.mp3");
            try (FileOutputStream os = new FileOutputStream(junk)) {
                os.write("this is not audio".getBytes());
            }
            String prepare;
            try {
                mp.setDataSource(junk.getPath());
                mp.prepare();
                prepare = "prepared";
            } catch (IOException e) {
                prepare = "IOException";
            }
            mp.release();
            say("mediaplayer errors: start before setDataSource " + early + ", junk file "
                    + prepare);
        }

        // MediaMetadataRetriever: the MP3's and the M4A's tags and durations.
        for (String[] f : new String[][] {{"mp3", "raw"}, {"m4a", "tone_aac.m4a"},
                {"flac", "tone_flac.flac"}}) {
            MediaMetadataRetriever r = new MediaMetadataRetriever();
            try (AssetFileDescriptor afd = f[1].equals("raw")
                    ? getResources().openRawResourceFd(R.raw.tone_mp3) : getAssets().openFd(f[1])) {
                r.setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
                String d = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION);
                say("retriever " + f[0] + ": title " + r.extractMetadata(
                        MediaMetadataRetriever.METADATA_KEY_TITLE)
                        + ", artist " + r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_ARTIST)
                        + ", album " + r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_ALBUM)
                        + ", mime " + r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_MIMETYPE)
                        + ", audio " + r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_HAS_AUDIO)
                        + ", duration " + (d != null && Math.abs(Integer.parseInt(d) - 800) <= 60
                                ? "ok" : "bad " + d));
            } catch (Exception e) {
                say("retriever " + f[0] + ": " + e);
            } finally {
                r.release();
            }
        }

        // 20-21. SoundPool: the WAV, then the Vorbis tone at twice its rate.
        {
            SoundPool pool = new SoundPool.Builder().setMaxStreams(2)
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_GAME).build()).build();
            CountDownLatch loaded = new CountDownLatch(2);
            List<String> statuses = Collections.synchronizedList(new ArrayList<>());
            on(handler, () -> {
                pool.setOnLoadCompleteListener((p, id, status) -> {
                    statuses.add(id + ":" + status);
                    loaded.countDown();
                });
                return null;
            });
            int wav = pool.load(this, R.raw.tone_wav, 1);
            int vorbis = pool.load(this, R.raw.tone_vorbis, 1);
            boolean all = loaded.await(5, TimeUnit.SECONDS);
            int s1 = pool.play(wav, 1f, 1f, 1, 0, 1f);
            Thread.sleep(800 + GAP_MS);
            int s2 = pool.play(vorbis, 1f, 1f, 1, 0, 2f);
            Thread.sleep(400 + GAP_MS);
            Collections.sort(statuses);
            say("soundpool: loaded " + all + " " + statuses + ", streams " + (s1 > 0) + " "
                    + (s2 > 0 && s2 != s1));
            pool.release();
        }
    }

    // --- Ringtone and RingtoneManager ---------------------------------------------------------

    static String tail(Uri uri) {
        return uri == null ? "null" : uri.getScheme() + ":.../" + uri.getLastPathSegment();
    }

    void ringtones() throws Exception {
        say("ringtone defaults: ringtone " + tail(RingtoneManager.getActualDefaultRingtoneUri(
                this, RingtoneManager.TYPE_RINGTONE)) + ", notification " + tail(
                RingtoneManager.getActualDefaultRingtoneUri(this,
                        RingtoneManager.TYPE_NOTIFICATION)) + ", alarm " + tail(
                RingtoneManager.getActualDefaultRingtoneUri(this, RingtoneManager.TYPE_ALARM)));
        Ringtone r = RingtoneManager.getRingtone(this,
                RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION));
        if (r == null) {
            say("ringtone notification: none");
            return;
        }
        r.play();
        Thread.sleep(150);
        boolean playing = r.isPlaying();
        long end = System.currentTimeMillis() + 3000;
        while (r.isPlaying() && System.currentTimeMillis() < end) Thread.sleep(20);
        say("ringtone notification: playing " + playing + ", ended " + !r.isPlaying());
        r.stop();
        Thread.sleep(GAP_MS);
    }

    // --- AAudio and OpenSL ES (lib/x86_64/libmedia_ndk_test.so) ------------------------------

    static native String aaudioCallbackTone(int hz, int ms);
    static native String aaudioWriteTone(int hz, int ms);
    static native String aaudioInput();
    static native String openslQueueTone(int hz, int ms);
    static native String openslFdPlay(int fd, long offset, long length);

    void ndkAudio() throws Exception {
        try {
            System.loadLibrary("media_ndk_test");
        } catch (UnsatisfiedLinkError e) {
            say("ndk: no libmedia_ndk_test.so: " + e.getMessage());
            return;
        }
        say("ndk aaudio callback: " + aaudioCallbackTone(1397, 500));
        Thread.sleep(GAP_MS);
        say("ndk aaudio write: " + aaudioWriteTone(1480, 400));
        Thread.sleep(GAP_MS);
        say("ndk aaudio input: " + aaudioInput());
        say("ndk opensl queue: " + openslQueueTone(1568, 600));
        Thread.sleep(GAP_MS);
        try (AssetFileDescriptor afd = getResources().openRawResourceFd(R.raw.tone_vorbis)) {
            say("ndk opensl fd: " + openslFdPlay(afd.getParcelFileDescriptor().getFd(),
                    afd.getStartOffset(), afd.getLength()));
        }
        Thread.sleep(GAP_MS);
    }
}
