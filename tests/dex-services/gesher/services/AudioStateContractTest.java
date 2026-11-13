/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.IntSupplier;

/** Actual Windows ART/JNI/engine contract. Run with GESHER_AUDIO=null; no endpoint is required. */
public final class AudioStateContractTest {
    private static int checks;
    private static Context context;
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static AudioTrack track(boolean isStatic, boolean floats) {
        return new AudioTrack.Builder()
                .setContext(context)
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(8000)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .setEncoding(floats ? AudioFormat.ENCODING_PCM_FLOAT : AudioFormat.ENCODING_PCM_16BIT)
                        .build())
                .setBufferSizeInBytes(isStatic ? 160 : 4096)
                .setTransferMode(isStatic ? AudioTrack.MODE_STATIC : AudioTrack.MODE_STREAM)
                .build();
    }
    private static int writeBounded(AudioTrack track, IntSupplier operation) throws Exception {
        AtomicInteger result = new AtomicInteger(Integer.MIN_VALUE);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread writer = new Thread(() -> {
            try { result.set(operation.getAsInt()); }
            catch (Throwable e) { failure.set(e); }
        }, "audio-state-contract-writer");
        writer.setDaemon(true);
        writer.start();
        writer.join(2000);
        if (writer.isAlive()) {
            track.release();
            writer.join(2000);
            throw new AssertionError("Inactive blocking write did not return within its deadline");
        }
        if (failure.get() != null) throw new AssertionError("Write failed", failure.get());
        checks++;
        return result.get();
    }
    private static void priming() throws Exception {
        for (int route = 0; route < 4; ++route) {
            for (boolean paused : new boolean[]{false, true}) {
                final AudioTrack track = track(false, route == 3);
                try {
                    check(track.getState() == AudioTrack.STATE_INITIALIZED, "Track initialized");
                    if (paused) { track.play(); track.pause(); }
                    int capacity = track.getBufferSizeInFrames();
                    check(capacity > 0 && capacity <= 1_000_000, "Bounded stream capacity");
                    if (route == 0) {
                        byte[] payload = new byte[capacity * 4];
                        check(writeBounded(track, () -> track.write(payload, 0, payload.length,
                                AudioTrack.WRITE_BLOCKING)) == capacity * 2, "Byte priming short count");
                    } else if (route == 1) {
                        short[] payload = new short[capacity * 2];
                        check(writeBounded(track, () -> track.write(payload, 0, payload.length,
                                AudioTrack.WRITE_BLOCKING)) == capacity, "Short priming short count");
                    } else if (route == 2) {
                        ByteBuffer payload = ByteBuffer.allocateDirect(capacity * 4);
                        check(writeBounded(track, () -> track.write(payload, payload.remaining(),
                                AudioTrack.WRITE_BLOCKING)) == capacity * 2, "Direct priming short count");
                        check(payload.position() == capacity * 2, "Direct buffer advances only committed bytes");
                    } else {
                        float[] payload = new float[capacity * 2];
                        check(writeBounded(track, () -> track.write(payload, 0, payload.length,
                                AudioTrack.WRITE_BLOCKING)) == capacity, "Float priming short count");
                    }
                    if (route != 3) {
                        byte[] sample = {0, 0};
                        check(writeBounded(track, () -> track.write(sample, 0, 2,
                                AudioTrack.WRITE_BLOCKING)) == 0, "Full inactive track reports zero progress");
                    }
                } finally { track.release(); }
            }
        }
    }
    private static void interrupts() throws Exception {
        for (int action = 0; action < 3; ++action) {
            final AudioTrack track = track(false, false);
            Thread writer = null;
            try {
                byte[] payload = new byte[4 * 1024 * 1024]; // Far longer than the bounded wait at 8 kHz.
                AtomicInteger result = new AtomicInteger(Integer.MIN_VALUE);
                AtomicReference<Throwable> failure = new AtomicReference<>();
                CountDownLatch entered = new CountDownLatch(1);
                track.play();
                writer = new Thread(() -> {
                    entered.countDown();
                    try { result.set(track.write(payload, 0, payload.length, AudioTrack.WRITE_BLOCKING)); }
                    catch (Throwable e) { failure.set(e); }
                }, "audio-state-active-writer");
                writer.setDaemon(true);
                writer.start();
                check(entered.await(2, TimeUnit.SECONDS), "Active writer entered");
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
                while (track.getPlaybackHeadPosition() == 0 && System.nanoTime() < deadline) Thread.sleep(10);
                check(track.getPlaybackHeadPosition() > 0 && writer.isAlive(),
                        "Actual mixer consumed frames while the long writer remained active");
                if (action == 0) track.pause();
                else if (action == 1) track.stop();
                else track.release();
                writer.join(2000);
                check(!writer.isAlive(), "Pause/stop/release interrupts a public blocking write");
                if (failure.get() != null) throw new AssertionError("Interrupted write threw", failure.get());
                check(result.get() > 0 && result.get() < payload.length && result.get() % 2 == 0,
                        "Interruption preserves partial whole-frame progress");
            } finally {
                track.release();
                if (writer != null) writer.join(2000);
            }
        }
    }
    private static void stopPriming() throws Exception {
        for (boolean waitForDrain : new boolean[]{false, true}) {
            AudioTrack track = track(false, false);
            try {
                int capacity = track.getBufferSizeInFrames();
                check(capacity >= 4 && capacity <= 1_000_000, "Bounded stop-priming capacity");
                short[] before = new short[capacity / 4];
                short[] after = new short[capacity / 2];
                check(track.write(before, 0, before.length, AudioTrack.WRITE_NON_BLOCKING) == before.length,
                        "Prime data for the first playback");
                track.play();
                track.stop();
                check(writeBounded(track, () -> track.write(after, 0, after.length,
                        AudioTrack.WRITE_BLOCKING)) == after.length,
                        "Stop permits priming available space for the next playback");
                if (waitForDrain) Thread.sleep(500);
                track.play();
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
                while (track.getPlaybackHeadPosition() < after.length && System.nanoTime() < deadline)
                    Thread.sleep(10);
                check(track.getPlaybackHeadPosition() >= after.length,
                        "Restart actually plays the newly primed data");
            } finally { track.release(); }
        }
    }
    private static void resets() throws Exception {
        AudioTrack streaming = track(false, false);
        try {
            check(streaming.setNotificationMarkerPosition(71) == AudioTrack.SUCCESS, "Set streaming marker");
            check(streaming.setPositionNotificationPeriod(53) == AudioTrack.SUCCESS, "Set streaming period");
            streaming.play();
            streaming.flush();
            check(streaming.getNotificationMarkerPosition() == 71
                    && streaming.getPositionNotificationPeriod() == 53, "Playing flush retains notification state");
            streaming.pause();
            streaming.flush();
            check(streaming.getNotificationMarkerPosition() == 0
                    && streaming.getPositionNotificationPeriod() == 0, "Inactive streaming flush clears notification state");
        } finally { streaming.release(); }
        AudioTrack fixed = track(true, false);
        try {
            short[] data = new short[80];
            check(fixed.write(data, 0, data.length) == data.length, "Load static data");
            check(fixed.setLoopPoints(0, data.length, -1) == AudioTrack.SUCCESS, "Set static infinite loop");
            fixed.play();
            Thread.sleep(100);
            fixed.stop();
            fixed.play();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
            while (fixed.getPlaybackHeadPosition() < data.length && System.nanoTime() < deadline) Thread.sleep(10);
            Thread.sleep(100);
            check(fixed.getPlaybackHeadPosition() == data.length, "Static replay stops at the end after stop cleared the loop");
        } finally { fixed.release(); }
    }
    public static void main(String[] args) throws Exception {
        // Invoke bootstrap reflectively so all API call sites compile against the public SDK alone.
        String root = System.getenv("ANDROID_ROOT");
        Class.forName("com.gesher.app.Main").getMethod("bootstrap", String.class, String.class, String.class)
                .invoke(null, "audio-state-contract", root + "/build.prop", root + "/framework/framework-res.apk");
        Class<?> services = Class.forName("com.gesher.server.Services");
        Object registry = services.getMethod("create", Collection.class).invoke(null, List.of());
        services.getMethod("installInProcess", Map.class).invoke(null, registry);
        // A standalone DEX has no installed Application from which AudioTrack can infer
        // attribution. Use the same real system Context as the other framework fixtures;
        // all playback calls still compile against and use the public SDK.
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadType.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object thread = constructor.newInstance();
        Method create = Class.forName("android.app.ContextImpl")
                .getDeclaredMethod("createSystemContext", threadType);
        create.setAccessible(true);
        context = (Context) create.invoke(null, thread);
        check(context.getAttributionSource() != null, "Real context supplies playback attribution");
        check(context.getDeviceId() == Context.DEVICE_ID_DEFAULT, "Default playback device");
        priming();
        interrupts();
        stopPriming();
        resets();
        System.out.println("AudioStateContractTest all passed: " + checks + " checks");
    }
}
