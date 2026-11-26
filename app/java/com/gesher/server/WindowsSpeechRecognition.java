/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * Windows' speech recognition (SAPI 5's in-process recognizer, through
 * compat/devices/speech_recognition_win.cc; docs/speech-recognition.md): the installed
 * recognizers and dictation sessions over PCM handed to them. The system server asks
 * which recognizers exist (whether the device has a recognition service,
 * {@link SttEngineApp}); the engine's own process (app/gesher-stt) recognizes the PCM
 * that its AudioRecord captured. Nothing here opens a microphone.
 */
public final class WindowsSpeechRecognition {
    /** Event kinds, as speech_recognition_win.h's EventKind. */
    public static final int EVENT_NONE = 0;
    public static final int EVENT_SOUND_START = 1;
    public static final int EVENT_SOUND_END = 2;
    public static final int EVENT_HYPOTHESIS = 3;
    public static final int EVENT_RECOGNITION = 4;
    public static final int EVENT_NO_MATCH = 5;
    public static final int EVENT_STREAM_END = 6;
    public static final int EVENT_FAILED = 7;

    /** Why a session did not start (speech_recognition_jni.cc's reasons). */
    public static final int REASON_WINDOWS = 1;
    public static final int REASON_NO_RECOGNIZER = 2;
    public static final int REASON_RECOGNIZER_FAILED = 3;

    /** A requested maximum is an upper bound, not a promise to return that many texts. */
    public static final int MAX_ALTERNATES = 64;

    private static final int RECOGNIZER_FIELDS = 4;

    private WindowsSpeechRecognition() {
    }

    /** One installed SAPI recognizer. */
    public static final class Recognizer {
        public final String id;
        public final String name;
        /** BCP-47 tags of the LCIDs the recognizer lists, such as "en-US". */
        public final List<String> languages;
        /** SAPI's default recognizer (Control Panel, Speech Recognition). */
        public final boolean isDefault;

        public Recognizer(String id, String name, List<String> languages, boolean isDefault) {
            this.id = id;
            this.name = name;
            this.languages = languages;
            this.isDefault = isDefault;
        }

        @Override
        public String toString() {
            return name + " " + languages + (isDefault ? " (default)" : "");
        }
    }

    /** A session that did not start: {@link #reason} and its HRESULT. */
    public static final class StartException extends Exception {
        public final int reason;
        public final int hr;

        StartException(int reason, int hr) {
            super((reason == REASON_NO_RECOGNIZER ? "no recognizer for the language"
                    : reason == REASON_RECOGNIZER_FAILED ? "the recognizer could not be loaded"
                    : "Windows speech recognition failed") + ": " + hresult(hr));
            this.reason = reason;
            this.hr = hr;
        }
    }

    public static String hresult(int hr) {
        return String.format("HRESULT 0x%08X", hr);
    }

    /** One event of a session. */
    public static final class Event {
        public final int kind;
        /** Recognition: the texts, best first; hypothesis: the partial text. Else empty. */
        public final String[] texts;
        /** Per text: Windows' confidence in [0, 1], or -1 when it reported none. */
        public final float[] confidences;
        /** {@link #EVENT_FAILED}: Windows' HRESULT. */
        public final int hr;

        public Event(int kind, String[] texts, float[] confidences, int hr) {
            this.kind = kind;
            this.texts = texts == null ? new String[0] : texts;
            this.confidences = confidences == null || confidences.length != this.texts.length
                    ? filled(this.texts.length) : confidences;
            this.hr = hr;
        }

        private static float[] filled(int n) {
            float[] c = new float[n];
            Arrays.fill(c, -1f);
            return c;
        }
    }

    /**
     * The installed recognizers; empty when Windows has none, or no SAPI at all (or a
     * process without these natives).
     */
    public static List<Recognizer> recognizers() {
        String[] flat;
        try {
            flat = nativeRecognizers(new int[1]);
        } catch (UnsatisfiedLinkError e) {
            return Collections.emptyList();
        }
        return recognizersOf(flat);
    }

    /** Reads nativeRecognizers' array: [id, name, languages (a,b), default] each. */
    static List<Recognizer> recognizersOf(String[] flat) {
        if (flat == null || flat.length % RECOGNIZER_FIELDS != 0) return Collections.emptyList();
        List<Recognizer> out = new ArrayList<>(flat.length / RECOGNIZER_FIELDS);
        for (int i = 0; i < flat.length; i += RECOGNIZER_FIELDS) {
            String id = flat[i], languages = flat[i + 2];
            if (id == null || id.isEmpty() || languages == null || languages.isEmpty()) continue;
            List<String> tags = new ArrayList<>();
            for (String tag : languages.split(",")) {
                if (!tag.isEmpty() && !tags.contains(tag)) tags.add(tag);
            }
            if (tags.isEmpty()) continue;
            out.add(new Recognizer(id, flat[i + 1] == null ? "" : flat[i + 1],
                    Collections.unmodifiableList(tags), "1".equals(flat[i + 3])));
        }
        return Collections.unmodifiableList(out);
    }

    /**
     * Starts dictation in {@code language} (a tag one of {@link #recognizers()} lists),
     * over 16-bit mono PCM at {@code sampleRate} that the caller writes.
     */
    public static Session start(String language, int sampleRate, boolean hypotheses,
            int maxAlternates) throws StartException {
        int[] out = new int[2];
        long handle;
        try {
            handle = nativeStart(language, sampleRate, hypotheses,
                    Math.min(MAX_ALTERNATES, Math.max(1, maxAlternates)), out);
        } catch (UnsatisfiedLinkError e) {
            throw new StartException(REASON_WINDOWS, 0x80004001);  // E_NOTIMPL
        }
        if (handle == 0) throw new StartException(out[1] == 0 ? REASON_WINDOWS : out[1], out[0]);
        return new Session(handle);
    }

    /**
     * A dictation session. {@link #write}, {@link #endAudio}, {@link #next} and
     * {@link #release} belong to one thread; {@link #cancel} may come from any thread.
     */
    public static final class Session {
        private final Object mLock = new Object();
        private long mHandle;

        Session(long handle) {
            mHandle = handle;
        }

        private long handle() {
            synchronized (mLock) {
                return mHandle;
            }
        }

        /** Appends PCM; false once the session takes no more audio. */
        public boolean write(short[] pcm, int count) {
            long h = handle();
            return h != 0 && nativeWrite(h, pcm, count);
        }

        /** No more audio: Windows recognizes what it has, then reports the stream's end. */
        public void endAudio() {
            long h = handle();
            if (h != 0) nativeEndAudio(h);
        }

        /** Stops now; no more events. */
        public void cancel() {
            synchronized (mLock) {
                if (mHandle != 0) nativeCancel(mHandle);
            }
        }

        /** The next event, waiting up to {@code timeoutMs}; EVENT_NONE when there is none. */
        public Event next(int timeoutMs) {
            long h = handle();
            if (h == 0) return new Event(EVENT_NONE, null, null, 0);
            Object[] out = new Object[2];
            int[] hr = new int[1];
            int kind = nativeNextEvent(h, timeoutMs, out, hr);
            return new Event(kind, (String[]) out[0], (float[]) out[1], hr[0]);
        }

        /** Releases Windows' recognizer; the session is unusable afterwards. */
        public void release() {
            long h;
            synchronized (mLock) {
                h = mHandle;
                mHandle = 0;
            }
            if (h != 0) nativeRelease(h);
        }
    }

    private static native String[] nativeRecognizers(int[] hrOut);
    private static native long nativeStart(String language, int sampleRate, boolean hypotheses,
            int maxAlternates, int[] out);
    private static native boolean nativeWrite(long handle, short[] pcm, int count);
    private static native void nativeEndAudio(long handle);
    private static native void nativeCancel(long handle);
    private static native void nativeRelease(long handle);
    private static native int nativeNextEvent(long handle, int timeoutMs, Object[] out, int[] hrOut);
}
