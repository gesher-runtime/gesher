/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Windows' speech synthesis (Windows.Media.SpeechSynthesis.SpeechSynthesizer, through
 * compat/devices/speech_win.cc; docs/tts.md): the installed voices, and utterances
 * rendered by them into WAV. The system server asks which voices exist (whether the
 * device has a text-to-speech engine, {@link TtsEngineApp}); the engine's own process
 * (app/gesher-tts) synthesizes. No audio is played here: Android's TextToSpeechService
 * plays or writes what the engine hands it.
 */
public final class WindowsSpeech {
    /** Await's results, as speech_win.h's kDone, kCanceled and kFailed. */
    public static final int DONE = 0;
    public static final int CANCELED = 1;
    public static final int FAILED = 2;

    /** Why a synthesis did not start (speech_jni.cc's reasons). */
    public static final int REASON_WINDOWS = 1;
    public static final int REASON_NO_SUCH_VOICE = 2;
    public static final int REASON_PROSODY_UNSUPPORTED = 3;

    /** Windows' SpeechSynthesizerOptions ranges (SpeakingRate, AudioPitch). */
    public static final double MIN_RATE = 0.5;
    public static final double MAX_RATE = 6.0;
    public static final double MIN_PITCH = 0.0;
    public static final double MAX_PITCH = 2.0;

    private static final int VOICE_FIELDS = 5;

    private WindowsSpeech() {
    }

    /** One installed Windows voice (VoiceInformation). */
    public static final class Voice {
        public final String id;
        public final String displayName;
        /** VoiceInformation.Language: a normalized BCP-47 tag, such as "he-IL". */
        public final String language;
        public final boolean female;
        /** SpeechSynthesizer.DefaultVoice is this voice. */
        public final boolean isDefault;

        public Voice(String id, String displayName, String language, boolean female,
                boolean isDefault) {
            this.id = id;
            this.displayName = displayName;
            this.language = language;
            this.female = female;
            this.isDefault = isDefault;
        }

        @Override
        public String toString() {
            return language + " " + displayName + (isDefault ? " (default)" : "");
        }
    }

    /** A failed Windows call: {@link #reason} and its HRESULT. */
    public static final class SpeechException extends Exception {
        public final int reason;
        public final int hr;

        SpeechException(int reason, int hr) {
            super((reason == REASON_NO_SUCH_VOICE ? "no such voice"
                    : reason == REASON_PROSODY_UNSUPPORTED ? "rate/pitch unsupported by Windows"
                    : "Windows speech synthesis failed") + ": " + hresult(hr));
            this.reason = reason;
            this.hr = hr;
        }
    }

    public static String hresult(int hr) {
        return String.format("HRESULT 0x%08X", hr);
    }

    /**
     * The installed voices; empty when Windows has none, or no speech synthesis at all
     * (an edition without WinRT media, or a process without these natives).
     */
    public static List<Voice> voices() {
        String[] flat;
        int[] hr = new int[1];
        try {
            flat = nativeVoices(hr);
        } catch (UnsatisfiedLinkError e) {
            return Collections.emptyList();
        }
        return voicesOf(flat);
    }

    /** Reads nativeVoices' array: [id, displayName, language, gender, default] each. */
    static List<Voice> voicesOf(String[] flat) {
        if (flat == null || flat.length % VOICE_FIELDS != 0) return Collections.emptyList();
        List<Voice> voices = new ArrayList<>(flat.length / VOICE_FIELDS);
        for (int i = 0; i < flat.length; i += VOICE_FIELDS) {
            String id = flat[i], language = flat[i + 2];
            if (id == null || id.isEmpty() || language == null || language.isEmpty()) continue;
            voices.add(new Voice(id, flat[i + 1] == null ? "" : flat[i + 1], language,
                    "1".equals(flat[i + 3]), "1".equals(flat[i + 4])));
        }
        return Collections.unmodifiableList(voices);
    }

    /**
     * Starts rendering {@code text} (plain text) with the voice {@code voiceId}, at
     * {@code rate} and {@code pitch} (within Windows' ranges above; the caller maps
     * Android's values). With {@code words}, word boundaries are requested.
     */
    public static Synthesis start(String text, String voiceId, double rate, double pitch,
            boolean words) throws SpeechException {
        if (!(rate >= MIN_RATE && rate <= MAX_RATE) || !(pitch >= MIN_PITCH && pitch <= MAX_PITCH)) {
            throw new IllegalArgumentException("rate " + rate + " or pitch " + pitch
                    + " outside Windows' ranges");
        }
        int[] out = new int[2];
        long handle;
        try {
            handle = nativeStart(text, voiceId, rate, pitch, words, out);
        } catch (UnsatisfiedLinkError e) {
            throw new SpeechException(REASON_WINDOWS, 0x80004001 /* E_NOTIMPL */);
        }
        if (handle == 0) throw new SpeechException(out[1] == 0 ? REASON_WINDOWS : out[1], out[0]);
        return new Synthesis(handle);
    }

    /**
     * One utterance being rendered. {@link #await} on the thread that started it;
     * {@link #cancel} from any thread; {@link #release} once.
     */
    public static final class Synthesis {
        private long mHandle;
        private boolean mAwaited;
        private boolean mAwaiting;
        private int mHr;

        Synthesis(long handle) {
            mHandle = handle;
        }

        /** Blocks until Windows is done: {@link #DONE}, {@link #CANCELED} or {@link #FAILED}. */
        public int await(long timeoutMs) {
            long handle;
            synchronized (this) {
                if (mHandle == 0 || mAwaited) throw new IllegalStateException("not awaitable");
                mAwaited = true;
                mAwaiting = true;
                handle = mHandle;
            }
            int[] hr = new int[1];
            int status = FAILED;
            try {
                // Not under the lock, so cancel() can reach Windows meanwhile; release()
                // waits for mAwaiting, so the handle stays valid.
                status = nativeAwait(handle, timeoutMs, hr);
            } finally {
                synchronized (this) {
                    mHr = hr[0];
                    mAwaiting = false;
                    notifyAll();
                }
            }
            return status;
        }

        public synchronized int hresult() {
            return mHr;
        }

        /** The stream Windows wrote: a RIFF WAV container. After {@link #DONE}. */
        public synchronized byte[] audio() {
            return mHandle == 0 ? null : nativeAudio(mHandle);
        }

        /** [startTicks (100 ns), first, last (inclusive)] per word. After {@link #DONE}. */
        public synchronized long[] words() {
            long[] words = mHandle == 0 ? null : nativeWords(mHandle);
            return words == null ? new long[0] : words;
        }

        /** Asks Windows to stop; harmless when it is done or released. */
        public synchronized void cancel() {
            if (mHandle != 0) nativeCancel(mHandle);
        }

        /** Frees the synthesis, after a concurrent {@link #await} has returned. */
        public synchronized void release() {
            boolean interrupted = false;
            while (mAwaiting) {
                try {
                    wait();
                } catch (InterruptedException e) {
                    interrupted = true;
                }
            }
            if (mHandle != 0) nativeRelease(mHandle);
            mHandle = 0;
            if (interrupted) Thread.currentThread().interrupt();
        }
    }

    // compat/devices/speech_jni.cc.
    private static native String[] nativeVoices(int[] hrOut);
    private static native long nativeStart(String text, String voiceId, double rate, double pitch,
            boolean words, int[] out);
    private static native int nativeAwait(long handle, long timeoutMs, int[] hrOut);
    private static native byte[] nativeAudio(long handle);
    private static native long[] nativeWords(long handle);
    private static native void nativeCancel(long handle);
    private static native void nativeRelease(long handle);
}
